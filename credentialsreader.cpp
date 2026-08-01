#include "credentialsreader.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDateTime>

QString CredentialsReader::claudeDir()
{
    return QDir::homePath() + "/.claude";
}

QJsonObject CredentialsReader::readJson()
{
    QFile file(claudeDir() + "/.credentials.json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

QString CredentialsReader::accessToken()
{
    return readJson().value("claudeAiOauth").toObject().value("accessToken").toString();
}

QString CredentialsReader::subscriptionType()
{
    // "pro", "max_5x", "max_20x"
    return readJson().value("claudeAiOauth").toObject().value("subscriptionType").toString();
}

bool CredentialsReader::isExpired()
{
    qint64 expiresAt = readJson().value("claudeAiOauth")
                           .toObject().value("expiresAt")
                           .toVariant()
                           .toLongLong();
    return QDateTime::currentMSecsSinceEpoch() >= expiresAt;
}
