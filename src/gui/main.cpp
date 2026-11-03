// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"
#include "theme.h"

#include "core/types.h"

#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("steam-library-mover"));
    QApplication::setApplicationDisplayName(QStringLiteral("Steam Library Mover"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.1"));
    QApplication::setOrganizationName(QStringLiteral("SteamLibraryMover"));
    QApplication::setDesktopFileName(QStringLiteral("steam-library-mover"));
    ui::applyTheme(ui::themeFromSettings());
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("steam-library-mover")));

    qRegisterMetaType<slm::SteamScan>("slm::SteamScan");
    qRegisterMetaType<slm::MovePlan>("slm::MovePlan");
    qRegisterMetaType<slm::MoveProgress>("slm::MoveProgress");
    qRegisterMetaType<slm::MoveResult>("slm::MoveResult");

    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty() || !QFileInfo(runtimeDir).isWritable())
        runtimeDir = QDir::tempPath();
    QLockFile lock(runtimeDir + QStringLiteral("/steam-library-mover.lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(200)) {
        QMessageBox::warning(nullptr, QStringLiteral("Steam Library Mover"),
                             QStringLiteral("Steam Library Mover is already running."));
        return 1;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
