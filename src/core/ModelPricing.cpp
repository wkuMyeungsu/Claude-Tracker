#include "ModelPricing.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QtGlobal>
#include <iterator>

namespace {

// 웹 검색 과금: $10 / 1,000회.
constexpr double WEB_SEARCH_COST = 10.0 / 1000.0;

// 계열별·버전별 요율. QMap<FamilyName, QMap<VersionName, ModelPricing>>
using PricingTree = QMap<QString, QMap<QString, ModelPricing>>;

// 매직 스태틱: 초기화가 한 번만, 스레드 안전하게 일어남을 C++11 이 보장한다.
// (예전의 `static QMap + static bool loaded` 조합은 이 보장을 받지 못해
//  워커 스레드 스캔과 메인 스레드 호출이 겹치면 QMap 동시 쓰기로 UB 였다.)
const PricingTree &pricingTree()
{
    static const PricingTree tree = []() {
        PricingTree t;
        QFile file(":/model_pricing.json");
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "[ModelPricing] model_pricing.json 을 열 수 없음 - 기본 요율 사용";
            return t;
        }
        const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
        for (auto familyIt = root.constBegin(); familyIt != root.constEnd(); ++familyIt) {
            if (familyIt.key().startsWith('_'))   // "_comment" 등 메타 키는 계열이 아니다
                continue;
            const QJsonObject versionObj = familyIt.value().toObject();
            QMap<QString, ModelPricing> versionMap;
            for (auto versionIt = versionObj.constBegin();
                 versionIt != versionObj.constEnd(); ++versionIt) {
                const QJsonObject rateObj = versionIt.value().toObject();
                ModelPricing p;
                p.inputRate      = rateObj["input_rate"].toDouble();
                p.outputRate     = rateObj["output_rate"].toDouble();
                p.cacheWriteRate = rateObj["cache_write_rate"].toDouble();
                p.cacheReadRate  = rateObj["cache_read_rate"].toDouble();
                // 1시간 캐시 요율이 없는 옛 파일이면 5분 요율로 폴백한다
                // (예전과 같은 동작 = 과소 계산이지만 크래시보다는 낫다).
                p.cacheWrite1hRate =
                    rateObj["cache_write_1h_rate"].toDouble(p.cacheWriteRate);
                // 값이 없으면 1.0 = fast mode 미지원 모델.
                p.fastMultiplier = rateObj["fast_multiplier"].toDouble(1.0);
                versionMap.insert(versionIt.key(), p);
            }
            t.insert(familyIt.key(), versionMap);
        }
        return t;
    }();
    return tree;
}

// 모델 ID 에서 버전 문자열을 뽑는다.
//   "claude-opus-4-1-20250805"   → "4.1"
//   "claude-3-5-sonnet-20241022" → "3.5"
//   "claude-opus-5"              → "5"
// 날짜 꼬리(8자리 이상 숫자)를 반드시 떼야 한다. 예전에는 모델 ID 전체를
// contains() 로 훑어서 "20250805" 안의 '5' 가 버전 키 "5" 에 걸렸고,
// 그 결과 Opus 4.1 이 Opus 5 요율(1/3 가격)로 계산됐다.
QString extractVersion(const QString &lowerName)
{
    QStringList nums;
    const QStringList tokens = lowerName.split('-', Qt::SkipEmptyParts);
    for (const QString &tok : tokens) {
        bool allDigits = true;
        for (const QChar &c : tok) {
            if (!c.isDigit()) { allDigits = false; break; }
        }
        if (!allDigits || tok.length() >= 8)   // 비숫자 토큰 / 날짜 꼬리는 제외
            continue;
        nums.append(tok);
    }
    return nums.join('.');
}

} // namespace

ModelPricing ModelPricingTable::forModel(const QString &modelName)
{
    // 요율 파일을 못 읽었을 때의 최종 폴백 (Sonnet 5 기준)
    ModelPricing defaultPricing;
    defaultPricing.inputRate        = 3.00;
    defaultPricing.outputRate       = 15.00;
    defaultPricing.cacheWriteRate   = 3.75;
    defaultPricing.cacheWrite1hRate = 6.00;
    defaultPricing.cacheReadRate    = 0.30;

    const PricingTree &tree = pricingTree();
    const QString lowerName = modelName.toLower();

    // 1단계: 계열(Family) 매칭
    QString matchedFamily;
    for (auto it = tree.constBegin(); it != tree.constEnd(); ++it) {
        if (lowerName.contains(it.key())) {
            matchedFamily = it.key();
            break;
        }
    }

    if (matchedFamily.isEmpty())
        return tree.value("sonnet").value("5", defaultPricing);

    // 2단계: 버전(Version) 매칭
    const QMap<QString, ModelPricing> versionMap = tree.value(matchedFamily);
    const QString version = extractVersion(lowerName);

    if (versionMap.contains(version))
        return versionMap.value(version);

    // 정확히 없으면 가장 긴 접두 일치 ("4.5.1" → "4.5", "3.5" 가 "3" 보다 우선)
    QString bestVersionKey;
    for (auto it = versionMap.constBegin(); it != versionMap.constEnd(); ++it) {
        if (version.startsWith(it.key()) && it.key().length() > bestVersionKey.length())
            bestVersionKey = it.key();
    }
    if (!bestVersionKey.isEmpty())
        return versionMap.value(bestVersionKey);

    // 계열은 맞췄지만 버전을 못 찾은 경우(= 아직 요율표에 없는 신모델)의 폴백.
    // QMap 은 키 정렬 상태이고 버전 키는 "3" < "3.5" < "4" < "4.8" < "5" 처럼
    // 문자열 순서가 곧 버전 순서이므로, 마지막 키가 그 계열의 최신 요율이다.
    // (두 자리 메이저 버전 "10" 이 등장하면 "3" 보다 앞서므로 그때 손봐야 한다.)
    if (!versionMap.isEmpty())
        return std::prev(versionMap.constEnd()).value();

    return defaultPricing;
}

double ModelPricingTable::costOf(const TokenRecord &r)
{
    ModelPricing p = forModel(r.model);
    if (r.fastMode && p.fastMultiplier > 1.0) {
        p.inputRate        *= p.fastMultiplier;
        p.outputRate       *= p.fastMultiplier;
        p.cacheWriteRate   *= p.fastMultiplier;
        p.cacheWrite1hRate *= p.fastMultiplier;
        p.cacheReadRate    *= p.fastMultiplier;
    }

    // 1시간 캐시는 5분 캐시의 1.6배다. Claude Code 는 1시간 캐시를 주로 쓰므로
    // 둘을 뭉뚱그리면 캐시 쓰기 비용이 구조적으로 과소 계산된다.
    const qint64 write1h = qBound<qint64>(0, r.cacheWrite1h, r.cacheWrite);
    const qint64 write5m = r.cacheWrite - write1h;

    return (r.input     * p.inputRate
          + r.output    * p.outputRate
          + write5m     * p.cacheWriteRate
          + write1h     * p.cacheWrite1hRate
          + r.cacheRead * p.cacheReadRate) / 1'000'000.0
         + r.webSearches * WEB_SEARCH_COST;
}
