#ifndef USAGESCANNER_H
#define USAGESCANNER_H

#include <QDateTime>
#include <QObject>
#include <QSet>
#include "usagedata.h"

class QFileSystemWatcher;

class UsageScanner : public QObject
{
    Q_OBJECT
public:
    explicit UsageScanner(QObject *parent = nullptr);

    void setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d);
    UsageData calcFromLocal() const;
    UsageData calcDeltaFromLocal(const QDateTime &sinceUtc) const;

    static qint64 planLimit5h();
    static qint64 planLimit7d();

signals:
    void localUsageUpdated(UsageData data);

private slots:
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);
    void refreshWatchList();

private:
    UsageData calcUsageForRange(const QDateTime &rangeStartUtc) const;

    QFileSystemWatcher *m_watcher;
    QSet<QString>       m_watchedFiles;
    QDateTime           m_lastKnownReset5h;
    QDateTime           m_lastKnownReset7d;
};

#endif // USAGESCANNER_H
