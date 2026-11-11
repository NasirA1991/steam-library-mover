// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"
#include "theme.h"

#include "core/instance_lock.h"

#include "core/types.h"

#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("steam-library-mover"));
    QApplication::setApplicationDisplayName(QStringLiteral("Steam Library Mover"));
    QApplication::setApplicationVersion(QString::fromLatin1(SLM_VERSION));
    QApplication::setOrganizationName(QStringLiteral("SteamLibraryMover"));
    QApplication::setDesktopFileName(QStringLiteral("steam-library-mover"));
    ui::applyTheme(ui::themeFromSettings());
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("steam-library-mover")));

    qRegisterMetaType<slm::SteamScan>("slm::SteamScan");
    qRegisterMetaType<slm::MovePlan>("slm::MovePlan");
    qRegisterMetaType<slm::MoveProgress>("slm::MoveProgress");
    qRegisterMetaType<slm::MoveResult>("slm::MoveResult");

    slm::InstanceLock lock;
    QString lockError;
    if (!lock.acquire(&lockError)) {
        QMessageBox::warning(nullptr, QStringLiteral("Steam Library Mover"), lockError);
        return 1;
    }

    MainWindow window;
    window.show();
    return app.exec();
}
