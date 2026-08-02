#include "UsageApiClient.h"
#include "CredentialsReader.h"
#include <cmath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

// CMakeLists 의 PROJECT_VERSION 에서 주입된다. 정의되지 않았다면 빌드 설정이
// 잘못된 것이고, 폴백을 두면 업데이트 체크가 조용히 오작동하므로 여기서 막는다.
#ifndef APP_VERSION
#  error "APP_VERSION is not defined - check target_compile_definitions in CMakeLists.txt"
#endif

static constexpr int POLL_INTERVAL_MS       = 5 * 60 * 1000;
static constexpr int INITIAL_FETCH_DELAY_MS = 500;
static constexpr int RETRY_DELAY_MS         = 30 * 1000;  // 실패 후 재시도 간격
static constexpr int MAX_RETRIES            = 3;          // 이 횟수 초과 시 정상 주기 복귀

static double normalizeUtilization(double utilization)
{
    return qBound(0.0, utilization / 100.0, 1.0);
}

// "1.2.3" 형태를 major/minor/patch 로 비교한다. 자리수가 모자라면 0 으로 채우고
// "1.2.3-rc1" 처럼 꼬리가 붙으면 각 자리의 선행 숫자만 취한다.
bool UsageApiClient::isNewerVersion(const QString &candidate, const QString &current)
{
    auto parse = [](const QString &v) {
        QVector<int> parts;
        const QStringList tokens = v.split('.');
        for (const QString &t : tokens) {
            int end = 0;
            while (end < t.size() && t.at(end).isDigit())
                ++end;
            parts.append(end > 0 ? t.left(end).toInt() : 0);
        }
        while (parts.size() < 3)
            parts.append(0);
        return parts;
    };

    const QVector<int> a = parse(candidate);
    const QVector<int> b = parse(current);
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i])
            return a[i] > b[i];
    }
    return false;
}

UsageApiClient::UsageApiClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_pollTimer(new QTimer(this))
{
    // QNetworkAccessManager::finished 는 이 매니저가 처리한 '모든' 응답에 대해
    // 발생한다. 여기에 onReplyFinished 를 걸면 checkForUpdates() 의 GitHub 응답까지
    // 사용량 응답으로 파싱되고, 먼저 실행되는 쪽이 readAll() 로 버퍼를 비워
    // 업데이트 체크가 빈 본문을 받는다. 요청별로 개별 연결할 것.
    m_pollTimer->setSingleShot(true);
    connect(m_pollTimer, &QTimer::timeout, this, &UsageApiClient::fetchUsage);
    scheduleNextPoll(INITIAL_FETCH_DELAY_MS);
}

QDateTime UsageApiClient::nextScheduledFetchAt() const
{
    return m_nextScheduledFetchAt;
}

int UsageApiClient::pollIntervalMs()
{
    return POLL_INTERVAL_MS;
}

void UsageApiClient::scheduleNextPoll(int delayMs)
{
    const int nextDelay = delayMs >= 0 ? delayMs : POLL_INTERVAL_MS;
    m_nextScheduledFetchAt = QDateTime::currentDateTime().addMSecs(nextDelay);
    m_pollTimer->start(nextDelay);
}

void UsageApiClient::fetchUsage()
{
    if (m_pending)
        return;

    scheduleNextPoll();

    if (CredentialsReader::isExpired()) {
        emit fetchFailed("OAuth token expired", false);
        return;
    }

    const QString token = CredentialsReader::accessToken();
    if (token.isEmpty()) {
        emit fetchFailed("No access token found", false);
        return;
    }

    QNetworkRequest req(QUrl("https://api.anthropic.com/api/oauth/usage"));
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
    req.setRawHeader("User-Agent", "ClaudeTray/" APP_VERSION);

    m_pending = true;
    emit fetchStarted();

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onReplyFinished(reply);
    });
}

void UsageApiClient::onReplyFinished(QNetworkReply *reply)
{
    m_pending = false;
    reply->deleteLater();

    const QByteArray body = reply->readAll();
    const QJsonObject root = QJsonDocument::fromJson(body).object();

    if (reply->error() != QNetworkReply::NoError || root.contains("error")) {
        const QString msg = root.contains("error")
            ? root["error"].toObject()["message"].toString()
            : reply->errorString();
        ++m_consecutiveFailures;
        if (m_consecutiveFailures <= MAX_RETRIES)
            scheduleNextPoll(RETRY_DELAY_MS);  // 5분 타이머를 30초로 앞당김
        const bool isNetwork = (reply->error() != QNetworkReply::NoError);
        emit fetchFailed(msg, isNetwork);
        return;
    }

    m_consecutiveFailures = 0;

    auto parseQuota = [](const QJsonObject &obj) -> QuotaInfo {
        QuotaInfo q;
        if (obj.isEmpty())
            return q;

        q.utilization = normalizeUtilization(obj["utilization"].toDouble());
        q.resetsAt = QDateTime::fromString(obj["resets_at"].toString(), Qt::ISODate);
        q.valid = true;
        return q;
    };

    UsageData data;
    data.fromApi = true;
    data.fetchedAt = QDateTime::currentDateTime();
    data.fiveHour = parseQuota(root["five_hour"].toObject());
    data.sevenDay = parseQuota(root["seven_day"].toObject());
    // 화면에는 안 나오지만 보정기가 Sonnet 전용 주간 창을 학습하는 데 쓴다.
    data.sevenDaySonnet = parseQuota(root["seven_day_sonnet"].toObject());

    if (root.contains("extra_usage")) {
        QJsonObject extraObj = root["extra_usage"].toObject();
        data.extraUsage.enabled = extraObj["is_enabled"].toBool();
        if (data.extraUsage.enabled) {
            double divisor = std::pow(10.0, extraObj["decimal_places"].toInt(2));
            data.extraUsage.limitDollars = extraObj["monthly_limit"].toDouble() / divisor;
            data.extraUsage.usedCredits = extraObj["used_credits"].toDouble() / divisor;
            data.extraUsage.utilization = qBound(0.0, extraObj["utilization"].toDouble() / 100.0, 1.0);
        }
    }

    emit usageFetched(data);
}

void UsageApiClient::checkForUpdates()
{
    QUrl url("https://api.github.com/repos/wkuMyeungsu/Claude-Tracker/releases/latest");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      "ClaudeTray-UpdateChecker/" APP_VERSION);

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return;

        QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        QString latestTag = root["tag_name"].toString();
        if (latestTag.isEmpty())
            return;

        QString cleanTag = latestTag;
        if (cleanTag.startsWith("v", Qt::CaseInsensitive)) {
            cleanTag = cleanTag.mid(1);
        }

        // 현재 버전은 CMake 의 PROJECT_VERSION 을 그대로 받는다.
        // 하드코딩하면 버전을 올릴 때 자기 자신을 최신으로 오인한다.
        if (isNewerVersion(cleanTag, APP_VERSION)) {
            QString downloadUrl = root["html_url"].toString();
            emit updateAvailable(latestTag, downloadUrl);
        }
    });
}
