#ifndef HOOKBRIDGE_H
#define HOOKBRIDGE_H

#include <QJsonObject>
#include <QObject>
#include <QString>

class QFileSystemWatcher;
class QTimer;

// Claude Code 훅 이벤트를 받아 "지금 클로드가 무엇을 하고 있는가"를 알려준다.
//
// .jsonl 대화 로그로는 이걸 알아낼 수 없다. 승인 대기 중인 tool_use 와 그냥
// 실행 중인 tool_use 는 로그상 완전히 동일한 모양이고, tool_result 는 툴이
// 끝난 뒤에야 append 되기 때문이다. 반면 Claude Code 의 PermissionRequest 훅은
// "권한 결정이 필요할 때"만 발생하므로 휴리스틱 없이 정확하다.
//
// 전달 경로:
//   Claude Code --(stdin JSON)--> ClaudeTray.exe --claude-tracker-hook
//     --> ~/.claude/tracker-state/<session_id>.json --> HookStateWatcher
//
// 세션마다 별도 파일을 쓰므로 동시에 여러 Claude Code 가 떠 있어도 서로의
// 쓰기를 덮어쓰지 않는다.
namespace HookBridge {

enum class AgentState {
    Unknown,          // 훅 미설치 등으로 알 수 없음 → 호출측이 폴백을 쓴다
    Idle,             // 대화 가능
    Running,          // 작업 중
    PendingApproval,  // 승인 대기
};

// 훅이 넘겨준 stdin JSON 을 읽어 상태 파일 한 개를 갱신하고 끝낸다.
// --claude-tracker-hook 로 실행됐을 때의 진입점.
int runHookMode();

QString stateDirPath();

// ~/.claude/settings.json 의 hooks 블록에 우리 훅이 등록돼 있는가.
bool hooksInstalled();
// 등록된 명령 경로가 지금 실행 중인 파일과 같은가. 재설치로 경로가 바뀌면
// 옛 경로가 남아 훅이 조용히 죽으므로, 다르면 다시 설치해야 한다.
bool hooksUpToDate();
// 기존 설정을 보존한 채 우리 훅만 병합/제거한다. 실패 시 false + errorOut.
bool installHooks(QString *errorOut = nullptr);
bool uninstallHooks(QString *errorOut = nullptr);

// 위 두 함수의 순수 계산 부분. settings.json 을 통째로 다루는 코드라
// (남의 훅·남의 설정을 지우면 안 된다) 파일 입출력과 떼어 두고 시험한다.
QJsonObject applyHooksToSettings(const QJsonObject &root, bool install, const QString &exePath);

// 상태 디렉터리를 감시하며 전체 세션을 하나의 상태로 합쳐 알려준다.
class StateWatcher : public QObject
{
    Q_OBJECT
public:
    explicit StateWatcher(QObject *parent = nullptr);

    AgentState state() const { return m_state; }
    // 감시를 (재)시작한다. 훅 설치 토글 직후 호출.
    void rescan();

signals:
    void stateChanged(HookBridge::AgentState state);

private:
    void reload();

    QFileSystemWatcher *m_watcher = nullptr;
    // 파일이 원자적 교체(temp→rename)로 바뀌면 감시가 끊기므로 다시 붙여야 한다.
    QTimer     *m_debounce = nullptr;
    AgentState  m_state    = AgentState::Unknown;
};

} // namespace HookBridge

#endif // HOOKBRIDGE_H
