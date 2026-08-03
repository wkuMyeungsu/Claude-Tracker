#include "CredentialsReader.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QDateTime>

QString CredentialsReader::claudeDir()
{
    // Claude Code 는 CLAUDE_CONFIG_DIR 로 설정 디렉터리를 통째로 옮길 수 있다.
    // 이걸 무시하고 ~/.claude 만 보면 그런 환경에서는 자격증명도 세션 로그도
    // 영영 못 찾아 화면이 계속 비어 있다. 여기서 한 번만 판단하면
    // settings.json·tracker-state·projects/ 경로가 모두 따라온다.
    const QByteArray configDir = qgetenv("CLAUDE_CONFIG_DIR");
    if (!configDir.isEmpty()) {
        QString path = QDir::fromNativeSeparators(QString::fromLocal8Bit(configDir));
        while (path.size() > 1 && path.endsWith('/'))
            path.chop(1);
        return path;
    }

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
