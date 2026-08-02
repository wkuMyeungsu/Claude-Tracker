#ifndef SESSIONLOGREADER_H
#define SESSIONLOGREADER_H

#include <QDateTime>
#include <QString>
#include <QVector>
#include "UsageTypes.h"

// Claude Code 가 ~/.claude/projects 아래에 쌓는 JSONL 대화 로그를 읽어
// 토큰 사용 레코드로 바꾼다. 집계·요율은 core 쪽 책임이다.
namespace SessionLogReader {

// projectsDir 아래 *.jsonl 을 모두(하위 디렉터리 포함) 읽어
// earliestUtc 이후 레코드를 돌려준다.
// recentModelOut 에는 (윈도우와 무관하게) 가장 최신 레코드의 모델명이 담긴다.
QVector<TokenRecord> readRecords(const QString &projectsDir,
                                 const QDateTime &earliestUtc,
                                 QString *recentModelOut = nullptr);

} // namespace SessionLogReader

#endif // SESSIONLOGREADER_H
