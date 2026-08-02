#include "SessionLogReader.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

QVector<TokenRecord> SessionLogReader::readRecords(const QString &projectsDir,
                                                   const QDateTime &earliestUtc,
                                                   QString *recentModelOut)
{
    QVector<TokenRecord> records;
    QSet<QString> seenKeys;
    QDateTime latestTs;
    QString   latestModel;

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

            // requestId 기준 중복 제거 (같은 응답이 thinking/tool_use 등 여러 줄로 기록됨)
            const QString rid = obj["requestId"].toString();
            const QString dedupKey = rid.isEmpty() ? obj["uuid"].toString() : rid;
            if (!dedupKey.isEmpty()) {
                if (seenKeys.contains(dedupKey))
                    continue;
                seenKeys.insert(dedupKey);
            }

            const QJsonObject message = obj["message"].toObject();
            QJsonObject usage = obj["usage"].toObject();
            if (usage.isEmpty())
                usage = message["usage"].toObject();
            if (usage.isEmpty())
                continue;

            const QString tsStr = obj["timestamp"].toString();
            QDateTime ts = QDateTime::fromString(tsStr, Qt::ISODateWithMs);
            if (!ts.isValid())
                ts = QDateTime::fromString(tsStr, Qt::ISODate);
            if (!ts.isValid())
                continue;
            // setTimeZone() 은 날짜·시각 필드를 그대로 두고 존만 바꿔 '재해석'한다.
            // "+09:00" 오프셋이 붙은 값이 오면 가리키는 순간이 통째로 어긋나므로
            // 반드시 변환(toUTC)해야 한다.
            ts = ts.toUTC();

            const QString model = message["model"].toString();

            // 최근 모델은 집계 윈도우와 무관하게 '전체 중 최신'을 쓴다.
            // 이것만 있으면 되므로 예전처럼 전체 레코드를 정렬할 필요가 없다.
            if (!latestTs.isValid() || ts > latestTs) {
                latestTs    = ts;
                latestModel = model;
            }

            // 집계 대상 밖이면 담지 않는다 (메모리·순회 비용 절감)
            if (earliestUtc.isValid() && ts < earliestUtc)
                continue;

            // ⚠ usage.iterations[] 는 폴백/재시도 등 시도별 내역이며 같은 토큰
            // 필드를 그대로 반복한다(실제 로그에서 input_tokens 가 정확히 2배로
            // 잡힌다). usage 최상위 값이 이미 그 시도들의 합계이므로
            // iterations 를 재귀 합산하면 즉시 2배 이상으로 부풀어 오른다.
            // 여기서는 반드시 최상위 값만 읽는다.
            TokenRecord rec;
            rec.ts         = ts;
            rec.model      = model;
            rec.input      = usage["input_tokens"].toVariant().toLongLong();
            rec.output     = usage["output_tokens"].toVariant().toLongLong();
            rec.cacheWrite = usage["cache_creation_input_tokens"].toVariant().toLongLong();
            rec.cacheRead  = usage["cache_read_input_tokens"].toVariant().toLongLong();

            // 캐시 쓰기 요율은 5분(x1.25)과 1시간(x2)이 다르다.
            // cache_creation 이 있으면 1시간분을 분리하고, 없는 옛 로그는
            // cacheWrite1h == 0 이라 전량 5분 요율로 계산된다.
            const QJsonObject cacheCreation = usage["cache_creation"].toObject();
            rec.cacheWrite1h =
                cacheCreation["ephemeral_1h_input_tokens"].toVariant().toLongLong();

            rec.webSearches =
                usage["server_tool_use"].toObject()["web_search_requests"]
                    .toVariant().toLongLong();
            rec.fastMode = (usage["speed"].toString() == "fast");

            records.append(rec);
        }
    }

    if (recentModelOut)
        *recentModelOut = latestModel;
    return records;
}
