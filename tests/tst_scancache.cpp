// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mountinfo.h"
#include "core/scancache.h"
#include "core/scanner.h"

#include "test_helpers.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace slm;

class ScanCacheTest : public QObject
{
    Q_OBJECT
private slots:
    void roundTrip();
};

void ScanCacheTest::roundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_CACHE_DIR", (temp.path() + QStringLiteral("/cache")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    const SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(scan.games.size(), 1);
    QVERIFY(ScanCache::save(scan, &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));

    SteamScan loaded;
    QVERIFY2(ScanCache::load(scan.root, &loaded, &error), qPrintable(error));
    QCOMPARE(loaded.root.path, scan.root.path);
    QCOMPARE(loaded.libraries.size(), scan.libraries.size());
    QCOMPARE(loaded.games.size(), scan.games.size());
    QCOMPARE(loaded.games.first().appId, scan.games.first().appId);
    QCOMPARE(loaded.games.first().name, scan.games.first().name);
    QCOMPARE(loaded.games.first().libraryPath, scan.games.first().libraryPath);
    QCOMPARE(loaded.games.first().sizeOnDisk, scan.games.first().sizeOnDisk);
    QCOMPARE(loaded.games.first().runtime, scan.games.first().runtime);
    QCOMPARE(loaded.libraryFoldersRaw, scan.libraryFoldersRaw);
    QCOMPARE(loaded.libraries.first().mount.freeBytes, scan.libraries.first().mount.freeBytes);
}

QTEST_GUILESS_MAIN(ScanCacheTest)
#include "tst_scancache.moc"
