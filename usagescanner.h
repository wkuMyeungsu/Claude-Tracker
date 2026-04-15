#ifndef USAGESCANNER_H
#define USAGESCANNER_H

#include <QDateTime>
#include <QObject>
#include <QSet>
#include "usagedata.h"

class QFileSystemWatcher;
class QTimer;

class UsageScanner : public QObject
{
    Q_OBJECT
public:
    explicit UsageScanner(QObject *parent = nullptr);

    // API 성공 시 reset 시각 힌트 제공 (로컬 추정 resetsAt 에 사용)
    void setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d);
    // API 성공 시 델타 기준 시각 설정 (마지막 API fetchedAt)
    void setDeltaStart(const QDateTime &since);

    // TrayApp::onFetchFailed 에서 메인 스레드 직접 호출용 (드물게 발생)
    UsageData calcFromLocal() const;
    UsageData calcDeltaFromLocal(const QDateTime &sinceUtc) const;

    static qint64 planLimit5h();
    static qint64 planLimit7d();

signals:
    // 파일 변경 감지 즉시 emit (디바운스 전) → TrayApp 투명도 제어에 사용
    void activityDetected();
    // 백그라운드 스캔 완료 시 emit
    // full  : 최근 5h/7d 전체 롤링 윈도우 결과
    // delta : m_deltaStart 이후 증분 결과 (hasDelta == false 이면 full 과 동일)
    void localUsageUpdated(UsageData full, UsageData delta, bool hasDelta);

private slots:
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);
    void refreshWatchList();
    void doScan();          // 디바운스 타임아웃 → 백그라운드 스캔 시작

private:
    // thread-safe: 멤버 변수 미사용, 인자로만 동작
    static UsageData calcUsageForRange(const QDateTime &rangeStartUtc,
                                       const QDateTime &reset5h,
                                       const QDateTime &reset7d);

    QFileSystemWatcher *m_watcher;
    QSet<QString>       m_watchedFiles;
    QDateTime           m_lastKnownReset5h;
    QDateTime           m_lastKnownReset7d;
    QDateTime           m_deltaStart;       // 마지막 API fetchedAt (UTC)
    QTimer             *m_debounceTimer;    // 파일 변경 이벤트 디바운스 (2s)
    bool                m_scanPending = false; // 백그라운드 스캔 진행 중 여부
};

#endif // USAGESCANNER_H
