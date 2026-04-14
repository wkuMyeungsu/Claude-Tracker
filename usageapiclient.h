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

signals:
    void usageFetched(UsageData data);
    void fetchFailed(QString reason);

public slots:
    void fetchUsage();

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    void scheduleNextPoll(int delayMs = -1);

    QNetworkAccessManager *m_nam;
    QTimer                *m_pollTimer;
    QDateTime              m_nextScheduledFetchAt;
    bool                   m_pending = false;
};

#endif // USAGEAPICLIENT_H
