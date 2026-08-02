#include "HookBridge.h"
#include "CredentialsReader.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>

#include <cstdio>

namespace HookBridge {

namespace {

// settings.json 안에서 "우리가 심은 훅"을 식별하는 표식. 명령 경로가 바뀌어도
// (재설치·이동) 이 인자로 옛 항목까지 찾아 정리한다.
const char *kHookFlag = "--claude-tracker-hook";

// 상태 파일이 이만큼 낡았는데도 idle 이 아니면, Claude Code 가 Stop 훅을 못 쏘고
// 죽은 것으로 본다. (강제 종료·크래시 후 🔄/🟡 가 영원히 남는 것을 막는다)
constexpr qint64 kStaleSecs   = 15 * 60;
// 완전히 잊어도 되는 시점. 이보다 오래된 파일은 지운다.
constexpr qint64 kForgetSecs  = 24 * 60 * 60;

QString settingsPath()
{
    return CredentialsReader::claudeDir() + "/settings.json";
}

QString stateToken(AgentState s)
{
    switch (s) {
    case AgentState::PendingApproval: return "pending_approval";
    case AgentState::Running:         return "running";
    default:                          return "idle";
    }
}

AgentState tokenToState(const QString &t)
{
    if (t == "pending_approval") return AgentState::PendingApproval;
    if (t == "running")          return AgentState::Running;
    return AgentState::Idle;
}

// 훅 이벤트 이름 → 그 시점의 상태.
// 로그로는 구분할 수 없는 "승인 대기"를 PermissionRequest 가 정확히 알려준다.
// 반환값이 Unknown 이면 이 이벤트로는 상태를 바꾸지 않는다.
AgentState eventToState(const QJsonObject &payload)
{
    const QString event = payload.value("hook_event_name").toString();

    if (event == "PermissionRequest")             return AgentState::PendingApproval;
    if (event == "UserPromptSubmit")              return AgentState::Running;
    if (event == "PostToolBatch"
        || event == "PostToolUse")                return AgentState::Running;
    if (event == "Stop" || event == "SessionStart") return AgentState::Idle;

    if (event == "Notification") {
        // Notification 은 권한 전용이 아니라서 종류를 봐야 한다.
        const QString kind = payload.value("notification_type").toString();
        if (kind == "permission_prompt") return AgentState::PendingApproval;
        if (kind == "idle_prompt")       return AgentState::Idle;
    }

    return AgentState::Unknown;
}

// 세션 id 를 파일명으로 쓸 수 있게 정리한다.
QString sessionFileName(const QString &sessionId)
{
    QString safe;
    safe.reserve(sessionId.size());
    for (const QChar c : sessionId) {
        if (c.isLetterOrNumber() || c == '-' || c == '_')
            safe.append(c);
    }
    if (safe.isEmpty())
        safe = "unknown";
    return safe + ".json";
}

bool writeJsonAtomic(const QString &path, const QJsonObject &obj, QString *errorOut)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    f.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (errorOut) *errorOut = f.errorString();
        return false;
    }
    return true;
}

// settings.json 의 훅 핸들러 한 개가 우리 것인가.
bool isOurHandler(const QJsonObject &handler)
{
    const QJsonArray args = handler.value("args").toArray();
    for (const QJsonValue &a : args) {
        if (a.toString() == QLatin1String(kHookFlag))
            return true;
    }
    return false;
}

QJsonObject ourHandler(const QString &exePath)
{
    QJsonObject h;
    h["type"]    = "command";
    h["command"] = exePath;
    h["args"]    = QJsonArray{ QLatin1String(kHookFlag) };
    // 훅이 클로드를 기다리게 하지 않는다. 상태 파일 한 줄 쓰는 게 전부다.
    h["async"]   = true;
    h["timeout"] = 5;
    return h;
}

struct HookEventSpec { const char *event; const char *matcher; };

// 턴당 스폰을 최소화한 조합이다. PostToolUse(툴마다) 대신 PostToolBatch(턴마다)를
// 쓰고 PreToolUse 는 아예 등록하지 않는다 → 보통 턴당 3회 + 승인 시 1회.
const HookEventSpec kHookEvents[] = {
    { "SessionStart",     nullptr },
    { "UserPromptSubmit", nullptr },
    { "PermissionRequest", "*" },
    { "PostToolBatch",    nullptr },
    { "Stop",             nullptr },
    { "SessionEnd",       nullptr },
    { "Notification",     "permission_prompt|idle_prompt" },
};

// 해당 이벤트 배열에서 우리 핸들러만 걷어낸다. 남의 훅은 건드리지 않는다.
QJsonArray stripOurs(const QJsonArray &groups)
{
    QJsonArray kept;
    for (const QJsonValue &gv : groups) {
        QJsonObject group    = gv.toObject();
        QJsonArray  handlers = group.value("hooks").toArray();

        QJsonArray keptHandlers;
        for (const QJsonValue &hv : handlers) {
            if (!isOurHandler(hv.toObject()))
                keptHandlers.append(hv);
        }
        // 우리 훅만 들어있던 그룹이면 그룹째 없앤다.
        if (keptHandlers.isEmpty())
            continue;

        group["hooks"] = keptHandlers;
        kept.append(group);
    }
    return kept;
}

QJsonObject readSettings()
{
    QFile f(settingsPath());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool applyHooks(bool install, QString *errorOut)
{
    QFile probe(settingsPath());
    const bool exists = probe.exists();
    if (exists && !probe.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QObject::tr("설정 파일을 읽을 수 없습니다: %1").arg(probe.errorString());
        return false;
    }
    const QByteArray raw = exists ? probe.readAll() : QByteArray();
    probe.close();

    QJsonParseError perr{};
    QJsonObject root;
    if (!raw.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            // 남의 설정을 통째로 날리는 것보다 실패하는 편이 낫다.
            if (errorOut)
                *errorOut = QObject::tr("settings.json 을 해석할 수 없습니다: %1").arg(perr.errorString());
            return false;
        }
        root = doc.object();
    }

    root = applyHooksToSettings(root, install, QCoreApplication::applicationFilePath());

    if (!QDir().mkpath(CredentialsReader::claudeDir())) {
        if (errorOut) *errorOut = QObject::tr("~/.claude 디렉터리를 만들 수 없습니다.");
        return false;
    }

    QString werr;
    if (!writeJsonAtomic(settingsPath(), root, &werr)) {
        if (errorOut) *errorOut = QObject::tr("settings.json 을 쓸 수 없습니다: %1").arg(werr);
        return false;
    }
    return true;
}

} // namespace

QString stateDirPath()
{
    return CredentialsReader::claudeDir() + "/tracker-state";
}

QJsonObject applyHooksToSettings(const QJsonObject &in, bool install, const QString &exePath)
{
    QJsonObject root  = in;
    QJsonObject hooks = root.value("hooks").toObject();

    for (const HookEventSpec &spec : kHookEvents) {
        // 우리 것만 걷어내고 다시 넣는다 → 두 번 켜도 중복되지 않는다.
        QJsonArray groups = stripOurs(hooks.value(QLatin1String(spec.event)).toArray());

        if (install) {
            QJsonObject group;
            if (spec.matcher)
                group["matcher"] = QLatin1String(spec.matcher);
            group["hooks"] = QJsonArray{ ourHandler(exePath) };
            groups.append(group);
        }

        // 남의 훅까지 비었으면 키 자체를 남기지 않는다 → 껐을 때 원래대로 돌아간다.
        if (groups.isEmpty())
            hooks.remove(QLatin1String(spec.event));
        else
            hooks[QLatin1String(spec.event)] = groups;
    }

    if (hooks.isEmpty())
        root.remove("hooks");
    else
        root["hooks"] = hooks;

    return root;
}

int runHookMode()
{
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly))
        return 0;
    const QByteArray raw = in.readAll();
    in.close();
    if (raw.isEmpty())
        return 0;

    const QJsonObject payload = QJsonDocument::fromJson(raw).object();
    if (payload.isEmpty())
        return 0;

    const QString dir = stateDirPath();
    if (!QDir().mkpath(dir))
        return 0;

    const QString path = dir + "/" + sessionFileName(payload.value("session_id").toString());
    const QString event = payload.value("hook_event_name").toString();

    // 세션이 끝나면 흔적을 남기지 않는다.
    if (event == QLatin1String("SessionEnd")) {
        QFile::remove(path);
        return 0;
    }

    const AgentState state = eventToState(payload);
    if (state == AgentState::Unknown)
        return 0;   // 우리가 신경 쓰지 않는 알림 종류

    QJsonObject out;
    out["state"] = stateToken(state);
    out["event"] = event;
    out["cwd"]   = payload.value("cwd").toString();
    out["ts"]    = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    writeJsonAtomic(path, out, nullptr);
    return 0;
}

bool hooksInstalled()
{
    const QJsonObject hooks = readSettings().value("hooks").toObject();
    // 승인 대기를 알려주는 이벤트가 살아 있는지만 보면 된다.
    const QJsonArray groups = hooks.value("PermissionRequest").toArray();
    for (const QJsonValue &gv : groups) {
        const QJsonArray handlers = gv.toObject().value("hooks").toArray();
        for (const QJsonValue &hv : handlers) {
            if (isOurHandler(hv.toObject()))
                return true;
        }
    }
    return false;
}

bool hooksUpToDate()
{
    const QString self = QCoreApplication::applicationFilePath();
    const QJsonObject hooks = readSettings().value("hooks").toObject();
    for (const QJsonValue &gv : hooks.value("PermissionRequest").toArray()) {
        for (const QJsonValue &hv : gv.toObject().value("hooks").toArray()) {
            const QJsonObject h = hv.toObject();
            if (isOurHandler(h))
                return h.value("command").toString() == self;
        }
    }
    return false;
}

bool installHooks(QString *errorOut)   { return applyHooks(true,  errorOut); }
bool uninstallHooks(QString *errorOut) { return applyHooks(false, errorOut); }

// ── StateWatcher ─────────────────────────────────────────────────────────────

StateWatcher::StateWatcher(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_debounce(new QTimer(this))
{
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(60);
    connect(m_debounce, &QTimer::timeout, this, &StateWatcher::reload);

    // 훅은 QSaveFile 로 원자적 교체(rename)를 하므로 디렉터리 감시로 잡힌다.
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { m_debounce->start(); });
    connect(m_watcher, &QFileSystemWatcher::fileChanged,      this, [this]() { m_debounce->start(); });
}

void StateWatcher::rescan()
{
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
    if (!m_watcher->files().isEmpty())
        m_watcher->removePaths(m_watcher->files());

    if (hooksInstalled()) {
        const QString dir = stateDirPath();
        QDir().mkpath(dir);
        m_watcher->addPath(dir);
    }
    reload();
}

void StateWatcher::reload()
{
    AgentState next = AgentState::Unknown;

    if (hooksInstalled()) {
        // 훅이 켜져 있으면 최소한 "대화 가능"은 안다. 세션 파일이 없다는 건
        // 아직 아무도 안 돌고 있다는 뜻이다.
        next = AgentState::Idle;

        const QDateTime now = QDateTime::currentDateTimeUtc();
        QDir dir(stateDirPath());
        const QFileInfoList files = dir.entryInfoList({"*.json"}, QDir::Files);

        QStringList watchFiles;
        for (const QFileInfo &fi : files) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly))
                continue;
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            f.close();

            const QDateTime ts = QDateTime::fromString(obj.value("ts").toString(), Qt::ISODate);
            const qint64 age = ts.isValid() ? ts.secsTo(now) : kForgetSecs + 1;

            if (age > kForgetSecs) {
                QFile::remove(fi.absoluteFilePath());
                continue;
            }
            watchFiles << fi.absoluteFilePath();

            AgentState s = tokenToState(obj.value("state").toString());
            // Stop 훅을 못 쏘고 죽은 세션이 상태를 붙잡고 있지 않게 한다.
            if (s != AgentState::Idle && age > kStaleSecs)
                s = AgentState::Idle;

            // 여러 세션이 동시에 떠 있으면 가장 사용자 손이 필요한 쪽을 보여준다.
            if (s == AgentState::PendingApproval)
                next = AgentState::PendingApproval;
            else if (s == AgentState::Running && next != AgentState::PendingApproval)
                next = AgentState::Running;
        }

        if (!watchFiles.isEmpty())
            m_watcher->addPaths(watchFiles);
    }

    if (next == m_state)
        return;
    m_state = next;
    emit stateChanged(m_state);
}

} // namespace HookBridge
