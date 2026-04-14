#include "usageapiclient.h"
#include "credentialsreader.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

static constexpr int POLL_INTERVAL_MS = 5 * 60 * 1000;
static constexpr int INITIAL_FETCH_DELAY_MS = 500;

static double normalizeUtilization(double utilization)
{
    if (utilization > 1.0)
        return qBound(0.0, utilization / 100.0, 1.0);
    return qBound(0.0, utilization, 1.0);
}

UsageApiClient::UsageApiClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_pollTimer(new QTimer(this))
{
    connect(m_nam, &QNetworkAccessManager::finished,
            this, &UsageApiClient::onReplyFinished);

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
        emit fetchFailed("OAuth token expired");
        return;
    }

    const QString token = CredentialsReader::accessToken();
    if (token.isEmpty()) {
        emit fetchFailed("No access token found");
        return;
    }

    QNetworkRequest req(QUrl("https://api.anthropic.com/api/oauth/usage"));
    req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
    req.setRawHeader("User-Agent", "ClaudeTray/1.0");

    m_pending = true;
    m_nam->get(req);
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
        emit fetchFailed(msg);
        return;
    }

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
    data.sevenDaySonnet = parseQuota(root["seven_day_sonnet"].toObject());

    emit usageFetched(data);
}
