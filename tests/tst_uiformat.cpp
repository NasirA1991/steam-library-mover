// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/uiformat.h"

#include <QDir>
#include <QtTest>

class UiFormatTest : public QObject
{
    Q_OBJECT
private slots:
    void fallbackDriveNames();
    void homeLibrary();
    void subtitleAndTooltip();
};

static slm::Library makeLibrary(const QString &path, const QString &label = {})
{
    slm::Library library;
    library.path = QDir::cleanPath(path);
    library.rawPath = path;
    library.label = label;
    library.mount.fsType = QStringLiteral("ntfs");
    library.mount.freeBytes = 100ull * 1024 * 1024 * 1024;
    library.mount.totalBytes = 200ull * 1024 * 1024 * 1024;
    return library;
}

void UiFormatTest::fallbackDriveNames()
{
    QCOMPARE(ui::driveDisplayName(makeLibrary(QStringLiteral("/mnt/games/kingston/SteamLibrary"))),
             QStringLiteral("Kingston"));
    QCOMPARE(ui::driveDisplayName(makeLibrary(QStringLiteral("/mnt/games/wdblack/SteamLibrary"))),
             QStringLiteral("WD Black"));
    QCOMPARE(ui::driveDisplayName(makeLibrary(QStringLiteral("/mnt/games/hdd2tb/SteamLibrary"))),
             QStringLiteral("HDD 2 TB"));
    QCOMPARE(ui::driveDisplayName(makeLibrary(QStringLiteral("/mnt/games/SteamLibrary"))),
             QStringLiteral("Games"));
    QCOMPARE(ui::driveDisplayName(makeLibrary(QStringLiteral("/mnt/games/SteamLibrary"),
                                              QStringLiteral("My Games"))),
             QStringLiteral("My Games"));
}

void UiFormatTest::homeLibrary()
{
    QCOMPARE(ui::driveDisplayName(
                 makeLibrary(QDir::homePath() + QStringLiteral("/.local/share/Steam"))),
             QStringLiteral("Home"));
}

void UiFormatTest::subtitleAndTooltip()
{
    const slm::Library library =
        makeLibrary(QStringLiteral("/mnt/games/kingston/SteamLibrary"));
    const QString subtitle = ui::driveSubtitle(library, 12);
    QVERIFY(subtitle.contains(QStringLiteral("NTFS")));
    QVERIFY(subtitle.contains(QStringLiteral("12 games")));
    const QString tooltip = ui::driveTooltip(library);
    QVERIFY(tooltip.contains(QStringLiteral("/mnt/games/kingston/SteamLibrary")));
    QVERIFY(tooltip.contains(QStringLiteral("Drive name: Kingston")));
    QVERIFY(tooltip.contains(QStringLiteral("Usage: 50%")));
    slm::MountInfo nvme;
    nvme.source = QStringLiteral("/dev/nvme0n1p2");
    QCOMPARE(ui::driveTypeLabel(nvme), QStringLiteral("NVMe SSD"));
}

QTEST_GUILESS_MAIN(UiFormatTest)
#include "tst_uiformat.moc"
