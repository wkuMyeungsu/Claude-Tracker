#ifndef SESSIONLOGWATCHER_H
#define SESSIONLOGWATCHER_H

#include <QDateTime>
#include <QObject>
#include "UsageTypes.h"
#include "QuotaCalibrator.h"

class QFileSystemWatcher;
class QTimer;

// Claude Code 세션 로그를 감시하다가, 변경이 멎으면 백그라운드 스레드에서
// 읽기(SessionLogReader) → 집계(UsageAggregator) 를 돌려 결과를 알린다.
//
// 이 클래스가 하는 일은 '언제 스캔할지' 뿐이다. 무엇을 어떻게 세는지는
// core 쪽에 있으므로 여기에는 계산 로직이 들어오지 않는다.
class SessionLogWatcher : public QObject
{
    Q_OBJECT
public:
    explicit SessionLogWatcher(QObject *parent = nullptr);

    // API 성공 시 reset 시각 힌트 제공 (로컬 추정 resetsAt 에 사용)
    void setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d);
    // API 성공 시 델타 기준 시각 설정 (마지막 API fetchedAt)
    void setDeltaStart(const QDateTime &since);
    // 학습된 보정 계수 주입. 스캔은 워커 스레드에서 돌므로 값 복사로 넘긴다.
    void setCalibration(const CalibrationSet &calibration);

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
    void localUsageUpdated(ScanResult result);

private slots:
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);
    void refreshWatchList();
    void onDebounceTimeout();   // 디바운스 만료 → idle 전환 + 스캔 요청

private:
    // thread-safe: 멤버 변수 미접근, 인자로만 동작
    static ScanResult scanLocal(const QDateTime &deltaStartUtc,
                                const QDateTime &reset5h,
                                const QDateTime &reset7d,
                                const CalibrationSet &calibration);

    QFileSystemWatcher *m_watcher;
    QDateTime           m_lastKnownReset5h;
    QDateTime           m_lastKnownReset7d;
    QDateTime           m_deltaStart;       // 마지막 API fetchedAt (UTC)
    CalibrationSet      m_calibration;      // 학습된 계수 (없으면 prior)
    QTimer             *m_debounceTimer;    // 파일 변경 이벤트 디바운스
    bool                m_scanPending  = false; // 백그라운드 스캔 진행 중 여부
    bool                m_rescanQueued = false; // 진행 중 스캔 완료 후 재실행 필요
};

#endif // SESSIONLOGWATCHER_H
