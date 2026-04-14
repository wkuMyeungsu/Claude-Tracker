#ifndef CREDENTIALSREADER_H
#define CREDENTIALSREADER_H

#include <QString>
#include <QJsonObject>

class CredentialsReader {
public:
    static QString claudeDir();
    static QString accessToken();
    static QString subscriptionType();
    static bool    isExpired();

private:
    static QJsonObject readJson();
};

#endif // CREDENTIALSREADER_H
