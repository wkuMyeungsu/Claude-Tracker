#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include "trayapp.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shobjidl.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    // 빌드마다 "실행 중입니다" 알림이 반복되지 않도록 앱 ID 고정
    SetCurrentProcessExplicitAppUserModelID(L"Anthropic.ClaudeTray");
#endif

    QApplication app(argc, argv);

    // 마지막 창이 닫혀도 앱 유지 (트레이 앱 필수 설정)
    app.setQuitOnLastWindowClosed(false);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        QMessageBox::critical(nullptr, "ClaudeTray",
                              "시스템 트레이를 사용할 수 없습니다.");
        return 1;
    }

    TrayApp tray;
    return app.exec();
}
