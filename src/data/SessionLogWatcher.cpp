#include "SessionLogWatcher.h"
#include "CredentialsReader.h"
#include "SessionLogReader.h"
#include "UsageAggregator.h"

#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QtConcurrent>

namespace {
constexpr int DEBOUNCE_MS  = 300;             // 파일 변경 디바운스 간격 (0.3초)
constexpr int WATCHLIST_MS = 5 * 60 * 1000;   // 감시 목록 갱신 간격
}

SessionLogWatcher::SessionLogWatcher(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounceTimer(new QTimer(this))
{
    qRegisterMetaType<ScanResult>("ScanResult");

    // 학습된 계수가 주입되기 전까지는 플랜 한도에서 유도한 prior 로 동작한다.
    // prior 는 예전 하드코딩 공식과 동일하므로 첫 실행 화면이 달라지지 않는다.
    const QString plan = CredentialsReader::subscriptionType();
    m_calibration = QuotaCalibrator::priorsFor(UsageAggregator::planLimit5h(plan),
                                               UsageAggregator::planLimit7d(plan));

    connect(m_watcher, &QFileSystemWatcher::directoryChanged,
            this, &SessionLogWatcher::onDirectoryChanged);
    connect(m_watcher, &QFileSystemWatcher::fileChanged,
            this, &SessionLogWatcher::onFileChanged);

    // 디바운스: DEBOUNCE_MS 동안 추가 이벤트가 없을 때만 스캔 실행
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(DEBOUNCE_MS);
    connect(m_debounceTimer, &QTimer::timeout, this, &SessionLogWatcher::onDebounceTimeout);

    refreshWatchList();

    // 앱 시작 시 기존 JSONL 파일 즉시 스캔 (파일 변경 이벤트 없이도 초기값 표시)
    QTimer::singleShot(200, this, &SessionLogWatcher::requestScan);

    // 5분마다 새 프로젝트 폴더 자동 감지 + 스캔 (QFileSystemWatcher 재귀 불가 우회,
    // Windows에서 watcher 이벤트 누락 시 폴백)
    // 디바운스 타이머가 아니라 requestScan 을 직접 부른다. 디바운스를 거치면
    // 파일 변경이 없었는데도 activityStopped 가 튀어 idle 로 잘못 전환된다.
    auto *watchTimer = new QTimer(this);
    connect(watchTimer, &QTimer::timeout, this, [this]() {
        refreshWatchList();
        requestScan();
    });
    watchTimer->start(WATCHLIST_MS);
}

void SessionLogWatcher::setWindowHints(const QDateTime &reset5h, const QDateTime &reset7d)
{
    if (reset5h.isValid())
        m_lastKnownReset5h = reset5h;
    if (reset7d.isValid())
        m_lastKnownReset7d = reset7d;
}

void SessionLogWatcher::setDeltaStart(const QDateTime &since)
{
    m_deltaStart = since.toUTC();
}

void SessionLogWatcher::setCalibration(const CalibrationSet &calibration)
{
    m_calibration = calibration;
}

// ── 파일 감시 이벤트 ──────────────────────────────────────────────────────────

void SessionLogWatcher::onDirectoryChanged(const QString &)
{
    refreshWatchList();
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: DEBOUNCE_MS 후 스캔
}

void SessionLogWatcher::onFileChanged(const QString &path)
{
    // Windows 에서 fileChanged 이벤트 후 파일이 watch list 에서 제거될 수 있어 재등록.
    // 단, '삭제'된 경우에도 같은 시그널이 오므로 존재 확인이 필요하다. 없는 경로에
    // addPath 를 부르면 실패하면서 Qt 경고만 남는다(삭제된 세션마다 반복 발생).
    if (!path.isEmpty() && QFile::exists(path))
        m_watcher->addPath(path);
    emit activityDetected();    // 즉시 알림 (투명도 제어용)
    m_debounceTimer->start();   // 타이머 리셋: DEBOUNCE_MS 후 스캔
}

void SessionLogWatcher::refreshWatchList()
{
    const QString projectsDir = CredentialsReader::claudeDir() + "/projects";

    // 감시 대상은 SessionLogReader 가 읽는 범위와 정확히 같아야 한다.
    // 예전에는 projects/<프로젝트> 한 단계만 훑었는데, 서브에이전트 로그는
    // projects/<프로젝트>/<세션UUID>/subagents/*.jsonl 에 쌓인다.
    // 읽기는 되지만 감시가 안 돼서 서브에이전트 사용량이 최대 5분(watchTimer
    // 주기)까지 화면에 안 나타났다. 이제 하위 디렉터리를 전부 따라간다.
    QSet<QString> wantDirs{projectsDir};
    QDirIterator dirs(projectsDir, QDir::Dirs | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    while (dirs.hasNext())
        wantDirs.insert(dirs.next());

    QSet<QString> wantFiles;
    QDirIterator files(projectsDir, {"*.jsonl"}, QDir::Files,
                       QDirIterator::Subdirectories);
    while (files.hasNext())
        wantFiles.insert(files.next());

    // 예전에는 추가만 하고 제거를 하지 않았다. 프로젝트를 지우거나 세션 로그가
    // 정리돼도 감시 목록에 경로가 영원히 남아 장기 실행 시 단조 증가했다.
    // QFileSystemWatcher 는 경로마다 OS 핸들을 소비하므로 한도를 넘기면
    // '새' 파일 감시가 조용히 실패하기 시작한다.
    // 이제 존재하는 경로 집합과 양방향으로 동기화한다.
    auto sync = [this](const QStringList &watched, const QSet<QString> &want) {
        QStringList stale;
        for (const QString &path : watched) {
            if (!want.contains(path))
                stale.append(path);
        }
        if (!stale.isEmpty())
            m_watcher->removePaths(stale);

        const QSet<QString> current(watched.constBegin(), watched.constEnd());
        QStringList fresh;
        for (const QString &path : want) {
            if (!current.contains(path))
                fresh.append(path);
        }
        if (!fresh.isEmpty())
            m_watcher->addPaths(fresh);
    };

    sync(m_watcher->directories(), wantDirs);
    sync(m_watcher->files(),       wantFiles);
}

// ── 백그라운드 스캔 ───────────────────────────────────────────────────────────

void SessionLogWatcher::onDebounceTimeout()
{
    // 디바운스 만료 = DEBOUNCE_MS 동안 파일 변경이 없었음 → 즉시 idle 전환
    emit activityStopped();
    requestScan();
}

void SessionLogWatcher::requestScan()
{
    if (m_scanPending) {
        // 진행 중인 스캔이 끝난 직후 한 번 더 돌린다. 예전처럼 디바운스 타이머를
        // 되감으면 그때마다 activityStopped 가 다시 나가는 부작용이 있었다.
        m_rescanQueued = true;
        return;
    }
    m_scanPending = true;

    // 멤버 변수를 람다에 복사 (스레드 안전)
    const QDateTime      hint5h      = m_lastKnownReset5h;
    const QDateTime      hint7d      = m_lastKnownReset7d;
    const QDateTime      deltaStart  = m_deltaStart;
    const CalibrationSet calibration = m_calibration;

    auto *watcher = new QFutureWatcher<ScanResult>(this);

    connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher]() {
        m_scanPending = false;
        const ScanResult result = watcher->result();
        watcher->deleteLater();

        emit localUsageUpdated(result);

        if (m_rescanQueued) {
            m_rescanQueued = false;
            requestScan();
        }
    });

    watcher->setFuture(QtConcurrent::run([hint5h, hint7d, deltaStart, calibration]() {
        return scanLocal(deltaStart, hint5h, hint7d, calibration);
    }));
}

// ── 스캔 오케스트레이션 (static: 멤버 변수 미접근 → 스레드 안전) ─────────────────

ScanResult SessionLogWatcher::scanLocal(const QDateTime &deltaStartUtc,
                                        const QDateTime &reset5h,
                                        const QDateTime &reset7d,
                                        const CalibrationSet &calibration)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime earliest =
        UsageAggregator::earliestRelevant(now, deltaStartUtc, reset7d);

    QString recentModel;
    const QVector<TokenRecord> records = SessionLogReader::readRecords(
        CredentialsReader::claudeDir() + "/projects", earliest, &recentModel);

    ScanResult result = UsageAggregator::aggregate(records, now, deltaStartUtc,
                                                   reset5h, reset7d, calibration);
    result.full.recentModel  = recentModel;
    result.delta.recentModel = recentModel;

    qDebug() << "[SessionLogWatcher] plan=" << CredentialsReader::subscriptionType()
             << "records=" << records.size()
             << "full5h%=" << qRound(result.full.fiveHour.utilization * 100.0)
             << "full7d%=" << qRound(result.full.sevenDay.utilization * 100.0)
             << "delta5h%=" << qRound(result.delta.fiveHour.utilization * 100.0)
             << "calibSamples5h=" << calibration.fiveHour.samples
             << "calibSamples7d=" << calibration.sevenDay.samples
             << "monthCost=" << result.full.extraUsage.usedCredits
             << "deltaCost=" << result.delta.extraUsage.usedCredits;

    return result;
}
