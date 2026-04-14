#include "usagescanner.h"
#include "credentialsreader.h"
#include <QDateTime>
#include <QDebug>
#include <QDirIterator>
#include <QFile>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <algorithm>

namespace {
const QMap<QString, qint64> PLAN_LIMITS_5H = {
    {"pro", 18'000'000LL},
    {"max_5x", 90'000'000LL},
    {"max_20x", 360'000'000LL},
};

const QMap<QString, qint64> PLAN_LIMITS_7D = {
    {"pro", 144'000'000LL},
    {"max_5x", 720'000'000LL},
    {"max_20x", 2'880'000'000LL},
};
}

struct TokenRecord {
    QDateTime ts;
    qint64 tokens = 0;
};

UsageScanner::UsageScanner(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &UsageScanner::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &UsageScanner::onFileChanged);

    refreshWatchList();

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &UsageScanner::refreshWatchList);
    timer->start(5 * 60 * 1000);
}

void UsageScanner::setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d)
{
    if (reset5h.isValid())
        m_lastKnownReset5h = reset5h;
    if (reset7d.isValid())
        m_lastKnownReset7d = reset7d;
}

void UsageScanner::refreshWatchList()
{
    const QString projectsDir = CredentialsReader::claudeDir() + "/projects";

    if (!m_watcher->directories().contains(projectsDir))
        m_watcher->addPath(projectsDir);

    QDirIterator dirs(projectsDir, QDir::Dirs | QDir::NoDotAndDotDot);
    while (dirs.hasNext()) {
        const QString dir = dirs.next();
        if (!m_watcher->directories().contains(dir))
            m_watcher->addPath(dir);

        QDirIterator files(dir, {"*.jsonl"}, QDir::Files);
        while (files.hasNext()) {
            const QString filePath = files.next();
            if (!m_watchedFiles.contains(filePath)) {
                m_watcher->addPath(filePath);
                m_watchedFiles.insert(filePath);
            }
        }
    }
}

void UsageScanner::onDirectoryChanged(const QString &)
{
    refreshWatchList();
    emit localUsageUpdated(calcFromLocal());
}

void UsageScanner::onFileChanged(const QString &)
{
    emit localUsageUpdated(calcFromLocal());
}

qint64 UsageScanner::planLimit5h()
{
    return PLAN_LIMITS_5H.value(CredentialsReader::subscriptionType(), 0);
}

qint64 UsageScanner::planLimit7d()
{
    return PLAN_LIMITS_7D.value(CredentialsReader::subscriptionType(), 0);
}

UsageData UsageScanner::calcFromLocal() const
{
    return calcUsageForRange(QDateTime());
}

UsageData UsageScanner::calcDeltaFromLocal(const QDateTime &sinceUtc) const
{
    return calcUsageForRange(sinceUtc.toUTC());
}

UsageData UsageScanner::calcUsageForRange(const QDateTime &rangeStartUtc) const
{
    const QString projectsDir = CredentialsReader::claudeDir() + "/projects";
    QSet<QString> seenUuids;
    QVector<TokenRecord> records;

    QDirIterator it(projectsDir, {"*.jsonl"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (!file.open(QIODevice::ReadOnly))
            continue;

        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty())
                continue;

            const QJsonObject obj = QJsonDocument::fromJson(line).object();
            if (obj["type"].toString() != "assistant")
                continue;

            const QString uid = obj["uuid"].toString();
            if (!uid.isEmpty()) {
                if (seenUuids.contains(uid))
                    continue;
                seenUuids.insert(uid);
            }

            const QJsonObject usage = obj["message"].toObject()["usage"].toObject();
            if (usage.isEmpty())
                continue;

            const QString tsStr = obj["timestamp"].toString();
            QDateTime ts = QDateTime::fromString(tsStr, Qt::ISODateWithMs);
            if (!ts.isValid())
                ts = QDateTime::fromString(tsStr, Qt::ISODate);
            if (!ts.isValid())
                continue;

            const qint64 tokens =
                usage["input_tokens"].toVariant().toLongLong() +
                usage["output_tokens"].toVariant().toLongLong() +
                usage["cache_creation_input_tokens"].toVariant().toLongLong() +
                usage["cache_read_input_tokens"].toVariant().toLongLong();

            records.append({ts.toUTC(), tokens});
        }
    }

    std::sort(records.begin(), records.end(),
              [](const TokenRecord &a, const TokenRecord &b) {
                  return a.ts < b.ts;
              });

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool deltaMode = rangeStartUtc.isValid();
    const QDateTime window5hStart = deltaMode ? rangeStartUtc : now.addSecs(-5LL * 3600);
    const QDateTime window7dStart = deltaMode ? rangeStartUtc : now.addDays(-7);

    qint64 rolling5hTokens = 0;
    qint64 rolling7dTokens = 0;

    for (const auto &record : records) {
        if (record.ts >= window7dStart)
            rolling7dTokens += record.tokens;
        if (record.ts >= window5hStart)
            rolling5hTokens += record.tokens;
    }

    const QString planType = CredentialsReader::subscriptionType();
    const qint64 limit5h = planLimit5h();
    const qint64 limit7d = planLimit7d();

    qDebug() << "[UsageScanner] plan=" << planType
             << "deltaMode=" << deltaMode
             << "rolling5h=" << rolling5hTokens
             << "rolling7d=" << rolling7dTokens
             << "limit5h=" << limit5h
             << "limit7d=" << limit7d;

    UsageData data;
    data.fromApi = false;
    data.fetchedAt = QDateTime::currentDateTime();

    if (rolling5hTokens > 0 || limit5h > 0) {
        data.fiveHour.rawTokens = rolling5hTokens;
        data.fiveHour.limitTokens = limit5h;
        data.fiveHour.resetsAt =
            (m_lastKnownReset5h.isValid() && m_lastKnownReset5h > now) ? m_lastKnownReset5h : QDateTime();
        data.fiveHour.valid = true;
        if (limit5h > 0)
            data.fiveHour.utilization = qMin(1.0, static_cast<double>(rolling5hTokens) / static_cast<double>(limit5h));
    }

    if (rolling7dTokens > 0 || limit7d > 0) {
        data.sevenDay.rawTokens = rolling7dTokens;
        data.sevenDay.limitTokens = limit7d;
        data.sevenDay.resetsAt =
            (m_lastKnownReset7d.isValid() && m_lastKnownReset7d > now) ? m_lastKnownReset7d : QDateTime();
        data.sevenDay.valid = true;
        if (limit7d > 0)
            data.sevenDay.utilization = qMin(1.0, static_cast<double>(rolling7dTokens) / static_cast<double>(limit7d));
    }

    return data;
}
