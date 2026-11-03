// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/health.h"
#include "core/library.h"
#include "core/mountinfo.h"
#include "core/scanner.h"

#include "test_helpers.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

using namespace slm;

class HealthTest : public QObject
{
    Q_OBJECT
private slots:
    void scanFindsStaleState();
    void quarantineMovesLockAndTemp();
};

void HealthTest::scanFindsStaleState()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString source = temp.path() + QStringLiteral("/libraryA");
    const QString steamapps = source + QStringLiteral("/steamapps");

    QVERIFY(testutil::writeFile(steamapps + QStringLiteral("/appmanifest_12345.acf.old.tmp"),
                                QByteArray("stale")));
    QVERIFY(testutil::writeFile(steamapps + QStringLiteral("/common/Test Game/game.lock"),
                                QByteArray("lock")));
    QVERIFY(testutil::writeFile(steamRoot + QStringLiteral("/steamapps/compatdata/12345/pfx.lock"),
                                QByteArray("lock")));
    QVERIFY(testutil::writeFile(steamapps + QStringLiteral("/common/Orphan Game/data.bin"),
                                QByteArray("orphan")));
    QVERIFY(testutil::writeFile(steamapps + QStringLiteral("/shadercache/99999/cache.bin"),
                                QByteArray("orphan")));
    QVERIFY(testutil::writeFile(steamapps + QStringLiteral("/workshop/content/99999/mod.bin"),
                                QByteArray("orphan")));
    QVERIFY(testutil::writeFile(steamRoot + QStringLiteral("/steamapps/compatdata/99999/pfx/system.reg"),
                                QByteArray("orphan")));
    QVERIFY(testutil::writeFile(steamapps + QStringLiteral("/common/.slm-backup-12345-test/game.dat"),
                                QByteArray("backup")));

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    const SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const QVector<HealthFinding> findings = LibraryHealth::scan(scan, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QSet<int> kinds;
    for (const HealthFinding &finding : findings)
        kinds.insert(static_cast<int>(finding.kind));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::StaleTempManifest)));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::StaleLockFile)));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::OrphanGameFolder)));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::OrphanShaderCache)));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::OrphanWorkshopContent)));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::OrphanPrefix)));
    QVERIFY(kinds.contains(static_cast<int>(HealthKind::SourceBackup)));
    for (const HealthFinding &finding : findings) {
        if (finding.kind == HealthKind::StaleTempManifest)
            QVERIFY(finding.quarantineable);
        if (finding.kind == HealthKind::SourceBackup)
            QVERIFY(!finding.quarantineable);
    }
}

void HealthTest::quarantineMovesLockAndTemp()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString steamapps = temp.path() + QStringLiteral("/libraryA/steamapps");
    const QString tempManifest = steamapps + QStringLiteral("/appmanifest_12345.acf.old.tmp");
    const QString lock = steamapps + QStringLiteral("/common/Test Game/game.lock");
    QVERIFY(testutil::writeFile(tempManifest, QByteArray("stale")));
    QVERIFY(testutil::writeFile(lock, QByteArray("lock")));

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    const SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVector<HealthFinding> findings = LibraryHealth::scan(scan, &error);
    QVector<HealthFinding> selected;
    for (const HealthFinding &finding : findings) {
        if (finding.quarantineable)
            selected.append(finding);
    }
    QVERIFY(selected.size() >= 2);

    QVERIFY2(LibraryHealth::quarantine(scan, selected, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(tempManifest));
    QVERIFY(!QFileInfo::exists(lock));
    QVERIFY(QDir(steamapps + QStringLiteral("/.slm-quarantine")).exists());
    QVERIFY(QDir(steamapps + QStringLiteral("/common/Test Game/.slm-quarantine")).exists());

    findings = LibraryHealth::scan(scan, &error);
    for (const HealthFinding &finding : findings) {
        if (finding.kind == HealthKind::StaleTempManifest
            || finding.kind == HealthKind::StaleLockFile)
            QVERIFY(!finding.path.endsWith(QStringLiteral("appmanifest_12345.acf.old.tmp")));
    }
}

QTEST_MAIN(HealthTest)
#include "tst_health.moc"
