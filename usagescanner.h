#ifndef USAGESCANNER_H
#define USAGESCANNER_H

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QVector>
#include "usagedata.h"
#include "usagecalibrator.h"

class QFileSystemWatcher;
class QTimer;

struct ModelPricing {
    double inputRate        = 0.0;
    double outputRate       = 0.0;
    double cacheWriteRate   = 0.0;   // 5분 캐시 쓰기 (입력가 x1.25)
    double cacheWrite1hRate = 0.0;   // 1시간 캐시 쓰기 (입력가 x2)
    double cacheReadRate    = 0.0;
    // usage.speed == "fast" 일 때 전 항목에 곱하는 배수.
    // 1.0 이면 fast mode 미지원 모델이라는 뜻이다.
    double fastMultiplier   = 1.0;
};

// JSONL 한 줄에서 뽑아낸 assistant 응답 1건의 토큰 사용량.
struct TokenRecord {
    QDateTime ts;                  // 항상 UTC
    QString   model;
    qint64    input        = 0;
    qint64    output       = 0;
    qint64    cacheWrite   = 0;    // 캐시 쓰기 총량 (5분 + 1시간)
    qint64    cacheRead    = 0;
    // 위 cacheWrite 중 1시간 캐시분. 요율이 5분의 1.6배라 반드시 분리해야 한다.
    // usage.cache_creation 이 없는 옛 로그에서는 0 → 전량 5분 요율로 계산된다.
    qint64    cacheWrite1h = 0;
    qint64    webSearches  = 0;    // server_tool_use.web_search_requests
    bool      fastMode     = false; // usage.speed == "fast"
};

// 스캔 1회 결과. 예전에는 full 과 delta 를 얻으려 전체 파일을 두 번 읽었지만
// 시작 시각만 다를 뿐이라 한 번 순회하며 두 누산기로 동시에 계산한다.
struct ScanResult {
    UsageData full;            // 현재 5h/7d 롤링 윈도우 전체
    UsageData delta;           // deltaStart 이후 증분 (hasDelta==false 면 full 과 동일)
    bool      hasDelta = false;

    // 보정기 학습용 특징벡터. full 윈도우 기준이며, API 가 알려주는
    // 같은 윈도우의 실제 utilization 과 짝지어 관측 1건이 된다.
    UsageFeatures full5hFeatures;
    UsageFeatures full7dFeatures;
    UsageFeatures full7dSonnetFeatures;   // Sonnet 계열만 (seven_day_sonnet 대응)
};

class UsageScanner : public QObject
{
    Q_OBJECT
public:
    explicit UsageScanner(QObject *parent = nullptr);

    static ModelPricing getPricingForModel(const QString &modelName);
    // 레코드 1건의 비용($). fast mode 배수와 1시간 캐시 요율을 반영한다.
    static double costOf(const TokenRecord &r);

    // API 성공 시 reset 시각 힌트 제공 (로컬 추정 resetsAt 에 사용)
    void setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d);
    // API 성공 시 델타 기준 시각 설정 (마지막 API fetchedAt)
    void setDeltaStart(const QDateTime &since);
    // 학습된 보정 계수 주입. 스캔은 워커 스레드에서 돌므로 값 복사로 넘긴다.
    void setCalibration(const CalibrationSet &calibration);

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
    // utilization 은 calibration 계수로 계산한다 (한도 나눗셈이 아니다).
    static ScanResult aggregate(const QVector<TokenRecord> &records,
                                const QDateTime &nowUtc,
                                const QDateTime &deltaStartUtc,
                                const QDateTime &reset5h,
                                const QDateTime &reset7d,
                                const CalibrationSet &calibration);
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
    void localUsageUpdated(ScanResult result);

private slots:
    void onDirectoryChanged(const QString &path);
    void onFileChanged(const QString &path);
    void refreshWatchList();
    void onDebounceTimeout();   // 디바운스 만료 → idle 전환 + 스캔 요청

private:
    // thread-safe: 멤버 변수 미사용, 인자로만 동작
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

Q_DECLARE_METATYPE(ScanResult)

#endif // USAGESCANNER_H
