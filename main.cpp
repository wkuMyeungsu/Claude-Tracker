#include <QApplication>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include "trayapp.h"

int main(int argc, char *argv[])
{
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
