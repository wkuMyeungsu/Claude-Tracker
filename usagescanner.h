#ifndef USAGESCANNER_H
#define USAGESCANNER_H

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QVector>
#include "usagedata.h"

class QFileSystemWatcher;
class QTimer;

struct ModelPricing {
    double inputRate = 0.0;
    double outputRate = 0.0;
    double cacheWriteRate = 0.0;
    double cacheReadRate = 0.0;
};

// JSONL 한 줄에서 뽑아낸 assistant 응답 1건의 토큰 사용량.
struct TokenRecord {
    QDateTime ts;              // 항상 UTC
    QString   model;
    qint64    input      = 0;
    qint64    output     = 0;
    qint64    cacheWrite = 0;
    qint64    cacheRead  = 0;
};

// 스캔 1회 결과. 예전에는 full 과 delta 를 얻으려 전체 파일을 두 번 읽었지만
// 시작 시각만 다를 뿐이라 한 번 순회하며 두 누산기로 동시에 계산한다.
struct ScanResult {
    UsageData full;            // 현재 5h/7d 롤링 윈도우 전체
    UsageData delta;           // deltaStart 이후 증분 (hasDelta==false 면 full 과 동일)
    bool      hasDelta = false;
};

class UsageScanner : public QObject
{
    Q_OBJECT
public:
    explicit UsageScanner(QObject *parent = nullptr);

    static ModelPricing getPricingForModel(const QString &modelName);

    // API 성공 시 reset 시각 힌트 제공 (로컬 추정 resetsAt 에 사용)
    void setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d);
    // API 성공 시 델타 기준 시각 설정 (마지막 API fetchedAt)
    void setDeltaStart(const QDateTime &since);

    static qint64 planLimit5h();
    static qint64 planLimit7d();

    // ── 아래 static 들은 순수 함수라 테스트에서 직접 호출한다 ───────────────────
    // 마지막 리셋 시각이 이미 과거면 주기를 더해 다음 리셋 시각을 추정한다.
    static QDateTime estimateNextReset(const QDateTime &last, qint64 periodSecs,
                                       const QDateTime &nowUtc);
    // 집계에 필요한 가장 이른 시각. 이보다 오래된 레코드는 파싱 단계에서 버린다.
    static QDateTime earliestRelevant(const QDateTime &nowUtc,
                                      const QDateTime &deltaStartUtc,
                                      const QDateTime &reset7d);
    // 파일 I/O 없이 레코드만으로 집계한다.
    static ScanResult aggregate(const QVector<TokenRecord> &records,
                                const QDateTime &nowUtc,
                                const QDateTime &deltaStartUtc,
                                const QDateTime &reset5h,
                                const QDateTime &reset7d,
                                qint64 limit5h,
                                qint64 limit7d);
    // projectsDir 아래 *.jsonl 을 모두 읽어 earliestUtc 이후 레코드를 돌려준다.
    // recentModelOut 에는 (윈도우와 무관하게) 가장 최신 레코드의 모델명이 담긴다.
    static QVector<TokenRecord> readRecords(const QString &projectsDir,
                                            const QDateTime &earliestUtc,
                                            QString *recentModelOut = nullptr);

public slots:
    // 파일 변경과 무관하게 백그라운드 스캔을 요청한다 (예: API 실패 폴백).
    // 항상 워커 스레드에서 돌므로 호출자를 블로킹하지 않는다.
    void requestScan();

signals:
    // 파일 변경 감지 즉시 emit (디바운스 전) → 활성 상태 전환
    void activityDetected();
    // 디바운스 만료 시 emit (DEBOUNCE_MS 동안 변경 없음) → 즉시 idle 전환
    void activityStopped();
    // 백그라운드 스캔 완료 시 emit
    // full  : 최근 5h/7d 전체 롤링 윈도우 결과
    // delta : m_deltaStart 이후 증분 결과 (hasDelta == false 이면 full 과 동일)
    void localUsageUpdated(UsageData full, UsageData delta, bool hasDelta);

private slots:
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);
    void refreshWatchList();
    void onDebounceTimeout();   // 디바운스 만료 → idle 전환 + 스캔 요청

private:
    // thread-safe: 멤버 변수 미사용, 인자로만 동작
    static ScanResult scanLocal(const QDateTime &deltaStartUtc,
                                const QDateTime &reset5h,
                                const QDateTime &reset7d);

    QFileSystemWatcher *m_watcher;
    QSet<QString>       m_watchedFiles;
    QDateTime           m_lastKnownReset5h;
    QDateTime           m_lastKnownReset7d;
    QDateTime           m_deltaStart;       // 마지막 API fetchedAt (UTC)
    QTimer             *m_debounceTimer;    // 파일 변경 이벤트 디바운스
    bool                m_scanPending  = false; // 백그라운드 스캔 진행 중 여부
    bool                m_rescanQueued = false; // 진행 중 스캔 완료 후 재실행 필요
};

#endif // USAGESCANNER_H
