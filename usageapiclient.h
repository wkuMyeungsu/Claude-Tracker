#ifndef USAGEAPICLIENT_H
#define USAGEAPICLIENT_H

#include <QObject>
#include "usagedata.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

class UsageApiClient : public QObject
{
    Q_OBJECT
public:
    explicit UsageApiClient(QObject *parent = nullptr);
    QDateTime nextScheduledFetchAt() const;
    static int pollIntervalMs();
    void checkForUpdates();

    // "1.2.0" > "1.1.9" 형태의 시맨틱 버전 비교. 순수 함수라 테스트에서 직접 부른다.
    static bool isNewerVersion(const QString &candidate, const QString &current);

signals:
    void fetchStarted();
    void usageFetched(UsageData data);
    void fetchFailed(QString reason, bool networkError);
    void updateAvailable(const QString &latestVersion, const QString &downloadUrl);

public slots:
    void fetchUsage();

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    void scheduleNextPoll(int delayMs = -1);

    QNetworkAccessManager *m_nam;
    QTimer                *m_pollTimer;
    QDateTime              m_nextScheduledFetchAt;
    bool                   m_pending            = false;
    int                    m_consecutiveFailures = 0;
};

#endif // USAGEAPICLIENT_H
