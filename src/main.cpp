#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include "HookBridge.h"
#include "TrayController.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shobjidl.h>
#endif

int main(int argc, char *argv[])
{
    // Claude Code 훅으로 불려온 경우. 트레이도 창도 띄우지 않고, stdin 으로 온
    // 이벤트를 상태 파일에 반영한 뒤 즉시 끝낸다. 턴마다 몇 번씩 실행되므로
    // QApplication(GUI 초기화)을 만들지 않는 것이 중요하다.
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--claude-tracker-hook") == 0) {
            QCoreApplication app(argc, argv);
            return HookBridge::runHookMode();
        }
    }

#ifdef Q_OS_WIN
    // 빌드마다 "실행 중입니다" 알림이 반복되지 않도록 앱 ID 고정.
    //
    // 네임스페이스는 반드시 이 앱 소유여야 한다. 예전 값 "Anthropic.ClaudeTray" 는
    // 서드파티 앱이 Anthropic 의 이름을 빌려 쓴 것이라, 같은 접두사를 쓰는 다른
    // 프로그램과 작업 표시줄 그룹·점프 목록·알림이 뒤섞일 수 있었다.
    // 형식은 CompanyName.ProductName (공백 불가, 128자 이하).
    SetCurrentProcessExplicitAppUserModelID(L"wkuMyeungsu.ClaudeTray");
#endif

    QApplication app(argc, argv);

    // 마지막 창이 닫혀도 앱 유지 (트레이 앱 필수 설정)
    app.setQuitOnLastWindowClosed(false);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "ClaudeTray",
                              "시스템 트레이를 사용할 수 없습니다.");
        return 1;
    }

    TrayController tray;
    return app.exec();
}
