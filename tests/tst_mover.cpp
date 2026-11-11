// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/copyengine.h"
#include "core/executor.h"
#include "core/fileops.h"
#include "core/journal.h"
#include "core/library.h"
#include "core/mountinfo.h"
#include "core/planner.h"
#include "core/scanner.h"
#include "core/vdf.h"

#include "core/process_check.h"

#include "test_helpers.h"

#include <QDir>
#include <atomic>
#include <thread>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>
#include <sys/stat.h>
#include <QFile>

using namespace slm;

class MoverTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void sharedPrefixMove();
    void relativeSymlinkMove();
    void doneWithWarningsIsFinished();
    void fullVerificationRejectsSameSizeCorruption();
    void verificationReportsProgress();
    void shaderCacheDestinationExistsIsWarning();
    void rollbackRejectsUnsafeJournal();
    void prefixMoveToRealDirectory();
    void cancelAndRollback();
    void revalidateRejectsChangedMount();
    void keepSourceBackup();
    void compatToolMetadata();
    void pauseAndResumeCopy();
    void resumeRepairsInterruptedVerification();
    void sequentialBatchRefreshesVdf();
    void cancellationInterruptsVerification();
    void ntfsInvalidNameBlocks();
    void ntfsAcknowledged();
    void unsupportedFileTypeBlocks();
    void pruneFinishedKeepsRecoverableOperations();
};

void MoverTest::initTestCase()
{
    if (::geteuid() == 0)
        QSKIP("Move/rollback tests must run as a normal user");
    setProcessInspectorForTesting([](const QStringList &, const QStringList &) { return ProcessUsage{}; });
    // Keep all journal/log/backup data inside the test temporary directory.
    qputenv("SLM_STATE_DIR", QDir::tempPath().toUtf8() + "/slm-test-state-"
                                  + qgetenv("USER"));
}

void MoverTest::sharedPrefixMove()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(scan.games.size(), 1);
    QCOMPARE(scan.games.first().appId, 12345u);
    QVERIFY2(!scan.games.first().artworkPath.isEmpty(), "local Steam librarycache artwork was not found");
    QVERIFY(scan.games.first().artworkPath.endsWith(QStringLiteral("header.jpg")));
    QVERIFY2(!scan.games.first().hasBlocker(), qPrintable(scan.games.first().issues.value(0).message));

    MoveOptions options;
    options.allowNtfsNative = true;
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(!plan.hasBlocker(), qPrintable(plan.blockers().isEmpty()
                                                ? QStringLiteral("no blocker")
                                                : plan.blockers().first().detail));
    const Component *prefix = plan.findComponent(ComponentKind::Prefix);
    QVERIFY(prefix);
    QVERIFY(prefix->skip);
    QCOMPARE(plan.findComponent(ComponentKind::Game)->bytes, 1048582ull);

    MoveExecutor executor;
    const MoveResult result = executor.execute(plan, mounts);
    QVERIFY2(result.ok, qPrintable(result.error));

    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/common/Test Game")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/common/Test Game/game.dat")));
    QVERIFY(QFileInfo::exists(
        steamRoot + QStringLiteral("/steamapps/compatdata/12345/pfx/system.reg")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/shadercache/12345/cache.bin")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/workshop/content/12345/mod.bin")));

    const auto vdf = VdfDocument::parse(testutil::readFile(steamRoot + QStringLiteral("/steamapps/libraryfolders.vdf")));
    QVERIFY(vdf);
    bool sourceHas = false;
    bool destHas = false;
    const VdfNode *libraries = vdf->object(QStringLiteral("libraryfolders"));
    QVERIFY(libraries);
    for (const VdfNode &library : libraries->children) {
        if (library.childValue(QStringLiteral("path")) == sourceLibrary)
            sourceHas = library.object(QStringLiteral("apps"))
                && library.object(QStringLiteral("apps"))->child(QStringLiteral("12345"));
        if (library.childValue(QStringLiteral("path")) == destLibrary)
            destHas = library.object(QStringLiteral("apps"))
                && library.object(QStringLiteral("apps"))->child(QStringLiteral("12345"));
    }
    QVERIFY(!sourceHas);
    QVERIFY(destHas);

}


void MoverTest::relativeSymlinkMove()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const QString payload = sourceLibrary + QStringLiteral("/steamapps/common/Test Game");
    QVERIFY(QDir().mkpath(payload + QStringLiteral("/sub")));
    QFile sibling(payload + QStringLiteral("/game.dat"));
    QVERIFY(sibling.open(QIODevice::ReadOnly));
    sibling.close();
    QCOMPARE(::symlink("game.dat", (payload + QStringLiteral("/sub/link-rel")).toUtf8().constData()), 0);
    QCOMPARE(::symlink("../game.dat", (payload + QStringLiteral("/link-up")).toUtf8().constData()), 0);

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    MoveOptions options;
    options.allowNtfsNative = true;
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(!plan.hasBlocker(), qPrintable(plan.blockers().isEmpty()
                                                ? QStringLiteral("no blocker")
                                                : plan.blockers().first().detail));

    MoveExecutor executor;
    const MoveResult result = executor.execute(plan, mounts);
    QVERIFY2(result.ok, qPrintable(result.error));

    const QString destPayload = destLibrary + QStringLiteral("/steamapps/common/Test Game");
    const auto rel = fsutil::readSymlinkTarget(destPayload + QStringLiteral("/sub/link-rel"));
    const auto up = fsutil::readSymlinkTarget(destPayload + QStringLiteral("/link-up"));
    QVERIFY(rel.has_value());
    QVERIFY(up.has_value());
    QCOMPARE(QString::fromUtf8(*rel), QStringLiteral("game.dat"));
    QCOMPARE(QString::fromUtf8(*up), QStringLiteral("../game.dat"));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/common/Test Game")));
}


void MoverTest::doneWithWarningsIsFinished()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    QString error;
    QVERIFY2(JournalStore::ensure(&error), qPrintable(error));
    QJsonObject journal;
    journal.insert(QStringLiteral("opId"), QStringLiteral("finished-with-warnings"));
    journal.insert(QStringLiteral("state"), QStringLiteral("done-with-warnings"));
    journal.insert(QStringLiteral("phase"), QStringLiteral("Done"));
    QVERIFY2(JournalStore::writeObject(
                 JournalStore::journalPath(QStringLiteral("finished-with-warnings")),
                 journal, &error), qPrintable(error));
    QVERIFY(MoveExecutor::unfinishedOperations().isEmpty());
}


void MoverTest::pruneFinishedKeepsRecoverableOperations()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    QString error;
    QVERIFY2(JournalStore::ensure(&error), qPrintable(error));

    const auto write = [&](const QString &opId, const QString &state, const QString &updated) {
        QJsonObject journal;
        journal.insert(QStringLiteral("opId"), opId);
        journal.insert(QStringLiteral("state"), state);
        journal.insert(QStringLiteral("updatedAt"), updated);
        return JournalStore::writeObject(JournalStore::journalPath(opId), journal, &error);
    };
    for (int i = 0; i < 25; ++i) {
        const QString id = QStringLiteral("finished-%1").arg(i, 2, 10, QLatin1Char('0'));
        QVERIFY2(write(id, QStringLiteral("done"),
                       QStringLiteral("2026-11-11T12:%1:00Z").arg(i, 2, 10, QLatin1Char('0'))),
                 qPrintable(error));
    }
    QVERIFY2(write(QStringLiteral("unfinished-op"), QStringLiteral("failed"),
                   QStringLiteral("2026-11-11T11:00:00Z")), qPrintable(error));

    QCOMPARE(JournalStore::pruneFinished(5), 20);
    const QList<QJsonObject> remaining = JournalStore::listOperations();
    QCOMPARE(remaining.size(), 6);
    int unfinished = 0;
    for (const QJsonObject &journal : remaining) {
        if (journal.value(QStringLiteral("state")).toString() == QStringLiteral("failed"))
            ++unfinished;
    }
    QCOMPARE(unfinished, 1);
    QVERIFY(QFileInfo::exists(JournalStore::journalPath(QStringLiteral("finished-24"))));
    QVERIFY(!QFileInfo::exists(JournalStore::journalPath(QStringLiteral("finished-00"))));
}

void MoverTest::fullVerificationRejectsSameSizeCorruption()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString source = temp.path() + QStringLiteral("/source");
    const QString destination = temp.path() + QStringLiteral("/destination");
    QVERIFY(QDir().mkpath(source));
    QFile file(source + QStringLiteral("/game.bin"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(1024 * 1024, 'A')), 1024 * 1024);
    file.close();

    QString error;
    QVERIFY2(CopyEngine::copyTree(source, destination, {}, {}, nullptr, &error),
             qPrintable(error));
    QFile corrupted(destination + QStringLiteral("/game.bin"));
    QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupted.write(QByteArray(1024 * 1024, 'B')), 1024 * 1024);
    corrupted.close();

    QVERIFY2(!CopyEngine::verifyTree(source, destination, true, &error), qPrintable(error));
    QVERIFY(error.contains(QStringLiteral("Checksum mismatch")));
    // Quick verification is intentionally only an explicit opt-out; it cannot
    // detect this corruption.
    QVERIFY(CopyEngine::verifyTree(source, destination, false, nullptr));
    QVERIFY(MoveOptions{}.fullVerify);
}


void MoverTest::verificationReportsProgress()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString source = temp.path() + QStringLiteral("/source");
    const QString destination = temp.path() + QStringLiteral("/destination");
    QVERIFY(QDir().mkpath(source));
    const QByteArray payload(512 * 1024, 'V');
    QVERIFY(testutil::writeFile(source + QStringLiteral("/a.bin"), payload));
    QVERIFY(testutil::writeFile(source + QStringLiteral("/b.bin"), payload));

    QString error;
    QVERIFY2(CopyEngine::copyTree(source, destination, {}, {}, nullptr, &error), qPrintable(error));
    quint64 lastBytes = 0;
    int calls = 0;
    QVERIFY2(CopyEngine::verifyTree(
                 source, destination, true, &error, {},
                 [&](quint64 bytes, const QString &) {
                     lastBytes = bytes;
                     ++calls;
                 }),
             qPrintable(error));
    QVERIFY(calls > 0);
    QCOMPARE(lastBytes, static_cast<quint64>(payload.size() * 4));
}

void MoverTest::rollbackRejectsUnsafeJournal()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    QString error;
    QVERIFY2(JournalStore::ensure(&error), qPrintable(error));
    const QString library = temp.path() + QStringLiteral("/libraryB");
    const QString outside = temp.path() + QStringLiteral("/outside");
    QVERIFY(QDir().mkpath(library + QStringLiteral("/steamapps")));
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(testutil::writeFile(outside + QStringLiteral("/keep.txt"), QByteArray("keep")));
    QVERIFY(testutil::writeFile(library + QStringLiteral("/steamapps/appmanifest_1.acf"),
                                QByteArray("manifest")));
    QVERIFY(testutil::writeFile(library + QStringLiteral("/steamapps/libraryfolders.vdf"),
                                QByteArray("vdf")));
    QVERIFY(testutil::writeFile(library + QStringLiteral("/steamapps/common/game.dat"),
                                QByteArray("dest")));

    QJsonObject journal;
    journal.insert(QStringLiteral("opId"), QStringLiteral("unsafe-op"));
    journal.insert(QStringLiteral("state"), QStringLiteral("in-progress"));
    journal.insert(QStringLiteral("phase"), QStringLiteral("CopyingGame"));
    journal.insert(QStringLiteral("manifest"), library + QStringLiteral("/steamapps/appmanifest_1.acf"));
    journal.insert(QStringLiteral("vdf"), library + QStringLiteral("/steamapps/libraryfolders.vdf"));
    journal.insert(QStringLiteral("destinationLibrary"), library);
    journal.insert(QStringLiteral("backupDir"), JournalStore::backupDir(QStringLiteral("unsafe-op")));
    journal.insert(QStringLiteral("components"), QJsonArray{
        QJsonObject{{QStringLiteral("kind"), static_cast<int>(ComponentKind::Game)},
                    {QStringLiteral("source"), temp.path() + QStringLiteral("/source")},
                    {QStringLiteral("destination"), outside + QStringLiteral("/keep.txt")}},
    });
    QVERIFY(!MoveExecutor::rollbackOperation(journal, &error));
    QVERIFY(error.contains(QStringLiteral("escapes")));
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));

    journal.insert(QStringLiteral("opId"), QStringLiteral("../unsafe"));
    QVERIFY(!MoveExecutor::rollbackOperation(journal, &error));
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));

    // If the VDF backup cannot be restored, nothing at the destination may be
    // deleted first. The source library must remain authoritative.
    QJsonObject vdfFailure = journal;
    vdfFailure.insert(QStringLiteral("opId"), QStringLiteral("vdf-failure"));
    vdfFailure.insert(QStringLiteral("backupDir"), JournalStore::backupDir(QStringLiteral("vdf-failure")));
    vdfFailure.insert(QStringLiteral("phase"), QStringLiteral("CopyingGame"));
    vdfFailure.insert(QStringLiteral("components"), QJsonArray{
        QJsonObject{{QStringLiteral("kind"), static_cast<int>(ComponentKind::Game)},
                    {QStringLiteral("source"), temp.path() + QStringLiteral("/source")},
                    {QStringLiteral("destination"), library + QStringLiteral("/steamapps/common/game.dat")}},
    });
    QVERIFY(!MoveExecutor::rollbackOperation(vdfFailure, &error));
    QVERIFY(error.contains(QStringLiteral("backup")));
    QVERIFY(QFileInfo::exists(library + QStringLiteral("/steamapps/common/game.dat")));
}


void MoverTest::shaderCacheDestinationExistsIsWarning()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const QString sourceShader = sourceLibrary + QStringLiteral("/steamapps/shadercache/12345/cache.bin");
    const QString destShader = destLibrary + QStringLiteral("/steamapps/shadercache/12345/cache.bin");
    QVERIFY(testutil::writeFile(destShader, QByteArray("existing destination cache")));

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    MoveOptions options;
    options.allowNtfsNative = true;
    const MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(!plan.hasBlocker(), qPrintable(plan.blockers().isEmpty()
                                                ? QStringLiteral("no blocker")
                                                : plan.blockers().first().detail));
    bool found = false;
    for (const Issue &issue : plan.issues) {
        if (issue.code == QStringLiteral("shadercache-destination-exists"))
            found = true;
    }
    QVERIFY(found);
    QVERIFY(QFileInfo::exists(destShader));
    QVERIFY(QFileInfo::exists(sourceShader));
    QVERIFY(!plan.findComponent(ComponentKind::ShaderCache));
}
void MoverTest::prefixMoveToRealDirectory()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    QVERIFY(QFile::remove(destLibrary + QStringLiteral("/steamapps/compatdata")));
    QDir().mkpath(destLibrary + QStringLiteral("/steamapps/compatdata"));

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());
    MoveOptions options;
    options.allowNtfsNative = true;
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(!plan.hasBlocker(), qPrintable(plan.blockers().isEmpty()
                                                ? QString()
                                                : plan.blockers().first().detail));
    const Component *prefix = plan.findComponent(ComponentKind::Prefix);
    QVERIFY(prefix);
    QVERIFY(!prefix->skip);

    MoveExecutor executor;
    const MoveResult result = executor.execute(plan, mounts);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/compatdata/12345/pfx/system.reg")));
    QVERIFY(!QFileInfo::exists(steamRoot + QStringLiteral("/steamapps/compatdata/12345")));
}

void MoverTest::cancelAndRollback()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());
    MoveOptions options;
    options.allowNtfsNative = true;
    const MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!plan.hasBlocker());

    MoveExecutor executor;
    const MoveResult result = executor.execute(plan, mounts, {}, []() { return true; });
    QVERIFY(result.cancelled);
    QVERIFY(QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/common/Test Game/game.dat")));
    QVERIFY(QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));

    const QList<QJsonObject> operations = MoveExecutor::unfinishedOperations();
    QVERIFY(!operations.isEmpty());
    QVERIFY(MoveExecutor::rollbackOperation(operations.first(), &error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(!QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/common/Test Game")));
    QVERIFY(QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
}

void MoverTest::revalidateRejectsChangedMount()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());
    MoveOptions options;
    options.allowNtfsNative = true;
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!plan.hasBlocker());
    plan.snapshot.destinationDevice++;
    QVERIFY(!MovePlanner::revalidate(plan, mounts, &error));
    QVERIFY(error.contains(QStringLiteral("filesystem")));

    MountInfo rootMount = testutil::makeMount(temp.path());
    MountInfo destMount = testutil::makeMount(destLibrary);
    const MountTable currentMounts = MountTable::fromMounts({rootMount, destMount});
    MovePlan fresh = MovePlanner::plan(scan, 12345, destLibrary, options, currentMounts, &error);
    QVERIFY(error.isEmpty());
    MountInfo readOnlyDest = destMount;
    readOnlyDest.readOnly = true;
    const MountTable readOnlyMounts = MountTable::fromMounts({rootMount, readOnlyDest});
    QVERIFY(!MovePlanner::revalidate(fresh, readOnlyMounts, &error));
    QVERIFY(error.contains(QStringLiteral("read-only")));
}

void MoverTest::keepSourceBackup()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());
    MoveOptions options;
    options.allowNtfsNative = true;
    options.keepSourceBackup = true;
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(!plan.hasBlocker(), qPrintable(plan.blockers().isEmpty()
                                                ? QString()
                                                : plan.blockers().first().detail));

    MoveExecutor executor;
    const MoveResult result = executor.execute(plan, mounts);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(result.cleanupErrors.isEmpty());
    QVERIFY(result.backupPaths.size() >= 2);
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/common/Test Game/game.dat")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/common/Test Game")));
    const QStringList gameBackups =
        QDir(sourceLibrary + QStringLiteral("/steamapps/common"))
            .entryList({QStringLiteral(".slm-backup-*")}, QDir::Dirs | QDir::Hidden);
    QVERIFY(!gameBackups.isEmpty());
    QVERIFY(QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/common/")
                              + gameBackups.first() + QStringLiteral("/game.dat")));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
}

void MoverTest::compatToolMetadata()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString config = QStringLiteral(
        "\"InstallConfigStore\"\n"
        "{\n"
        "\t\"Software\"\n"
        "\t{\n"
        "\t\t\"Valve\"\n"
        "\t\t{\n"
        "\t\t\t\"Steam\"\n"
        "\t\t\t{\n"
        "\t\t\t\t\"CompatToolMapping\"\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\t\"12345\"\n"
        "\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\"name\"\t\t\"proton_9\"\n"
        "\t\t\t\t\t}\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n"
        "}\n");
    QVERIFY(testutil::writeFile(steamRoot + QStringLiteral("/config/config.vdf"),
                                config.toUtf8()));
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    const SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(scan.games.size(), 1);
    QCOMPARE(scan.games.first().runtime, RuntimeKind::Proton);
    QCOMPARE(scan.games.first().protonVersion, QStringLiteral("Proton 9.0"));
}

void MoverTest::pauseAndResumeCopy()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString source = temp.path() + QStringLiteral("/source");
    const QString destination = temp.path() + QStringLiteral("/destination");
    QDir().mkpath(source);
    QVERIFY(testutil::writeFile(source + QStringLiteral("/data.bin"), QByteArray(8 * 1024 * 1024, 'p')));

    std::atomic_bool paused{true};
    std::atomic_bool cancelled{false};
    bool ok = false;
    QString error;
    std::thread worker([&]() {
        ok = CopyEngine::copyTree(
            source, destination, {},
            [&]() { return cancelled.load(std::memory_order_relaxed); }, nullptr, &error,
            [&]() { return paused.load(std::memory_order_relaxed); });
    });
    QTest::qWait(250);
    QVERIFY2(!QFileInfo::exists(destination + QStringLiteral("/data.bin")),
             "copy started while paused");
    paused.store(false, std::memory_order_relaxed);
    worker.join();
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFileInfo::exists(destination + QStringLiteral("/data.bin")));
}

void MoverTest::cancellationInterruptsVerification()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString source = temp.path() + QStringLiteral("/source");
    const QString destination = temp.path() + QStringLiteral("/destination");
    QDir().mkpath(source);
    QVERIFY(testutil::writeFile(source + QStringLiteral("/game.bin"),
                                QByteArray(2 * 1024 * 1024, 'V')));
    QString error;
    QVERIFY2(CopyEngine::copyTree(source, destination, {}, {}, nullptr, &error), qPrintable(error));
    std::atomic_bool cancelled{true};
    QVERIFY(!CopyEngine::verifyTree(source, destination, true, &error,
                                    [&]() { return cancelled.load(std::memory_order_relaxed); }));
    QVERIFY(error.contains(QStringLiteral("Cancelled"), Qt::CaseInsensitive));
}

void MoverTest::resumeRepairsInterruptedVerification()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const QString destPayload = destLibrary + QStringLiteral("/steamapps/common/Test Game");
    const QString destFile = destPayload + QStringLiteral("/game.dat");
    const QByteArray original = testutil::readFile(
        sourceLibrary + QStringLiteral("/steamapps/common/Test Game/game.dat"));
    QVERIFY(!original.isEmpty());

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    MoveOptions options;
    options.allowNtfsNative = true;
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!plan.hasBlocker());

    MoveExecutor executor;
    bool cancelAtVerify = false;
    const MoveResult interrupted = executor.execute(
        plan, mounts,
        [&](const MoveProgress &progress) {
            if (progress.phase == Phase::Verifying)
                cancelAtVerify = true;
        },
        [&]() { return cancelAtVerify; });
    QVERIFY(interrupted.cancelled);
    QVERIFY(QFileInfo::exists(destFile));

    const QList<QJsonObject> operations = MoveExecutor::unfinishedOperations();
    QVERIFY(!operations.isEmpty());
    const QJsonObject journal = operations.first();
    QVERIFY(journal.value(QStringLiteral("phase")).toString() == phaseName(Phase::Verifying));
    QVERIFY2(MoveExecutor::canResume(journal, &error), qPrintable(error));

    QJsonObject committed = journal;
    committed.insert(QStringLiteral("phase"), phaseName(Phase::WritingManifest));
    QVERIFY(!MoveExecutor::canResume(committed, &error));
    QVERIFY(error.contains(QStringLiteral("commit")));

    QJsonObject wrongBackup = journal;
    wrongBackup.insert(QStringLiteral("backupDir"),
                       slm::JournalStore::backupDir(QStringLiteral("another-operation")));
    QVERIFY(!MoveExecutor::canResume(wrongBackup, &error));
    QVERIFY(error.contains(QStringLiteral("backup")));

    // Corrupt one destination file. Resuming must detect this, remove the bad
    // component, copy it again, and still complete the transaction.
    QFile corrupted(destFile);
    QVERIFY(corrupted.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupted.write(QByteArray(original.size(), 'Z')), original.size());
    corrupted.close();

    const MoveResult resumed = executor.resumeOperation(journal, mounts);
    QVERIFY2(resumed.ok, qPrintable(resumed.error));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QCOMPARE(testutil::readFile(destFile), original);
}

void MoverTest::sequentialBatchRefreshesVdf()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    // Add a second game and manifest to the source library.
    QDir().mkpath(sourceLibrary + QStringLiteral("/steamapps/common/Test Game 2"));
    QVERIFY(testutil::writeFile(sourceLibrary + QStringLiteral("/steamapps/common/Test Game 2/game.dat"),
                                QByteArray(2048, 'B')));
    QVERIFY(testutil::writeFile(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12346.acf"),
                                testutil::manifestText(12346, "Test Game 2", "Test Game 2", 4, 2048)
                                    .toUtf8()));

    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(scan.findGame(12345));
    QVERIFY(scan.findGame(12346));

    MoveOptions options;
    options.allowNtfsNative = true;
    MovePlan first = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!first.hasBlocker());
    MoveExecutor executor;
    MoveResult result = executor.execute(first, mounts);
    QVERIFY2(result.ok, qPrintable(result.error));

    MovePlan second = MovePlanner::plan(scan, 12346, destLibrary, options, mounts, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!second.hasBlocker());
    QVERIFY(MovePlanner::refreshVdfSnapshot(second, &error));
    result = executor.execute(second, mounts);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/common/Test Game 2/game.dat")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12346.acf")));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12346.acf")));
}

void MoverTest::ntfsInvalidNameBlocks()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    QVERIFY(testutil::writeFile(sourceLibrary + QStringLiteral("/steamapps/common/Test Game/bad:name"),
                                QByteArray("bad")));

    MountInfo rootMount = testutil::makeMount(temp.path());
    MountInfo destMount = testutil::makeMount(destLibrary, QStringLiteral("ntfs3"), FsClass::Ntfs);
    const MountTable mounts = MountTable::fromMounts({rootMount, destMount});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());

    MoveOptions options;
    options.allowNtfsNative = true;
    const MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY(error.isEmpty());
    bool found = false;
    for (const Issue &issue : plan.issues) {
        if (issue.code == QStringLiteral("ntfs-invalid-name"))
            found = true;
    }
    QVERIFY(found);
}

void MoverTest::unsupportedFileTypeBlocks()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    const QString fifo = sourceLibrary + QStringLiteral("/steamapps/common/Test Game/pipe");
    QCOMPARE(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    MovePlan plan = MovePlanner::plan(scan, 12345, destLibrary, MoveOptions{}, mounts, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(plan.hasBlocker());
    QVERIFY(std::any_of(plan.issues.cbegin(), plan.issues.cend(), [](const Issue &issue) {
        return issue.code == QStringLiteral("unsupported-file-type");
    }));
}

void MoverTest::ntfsAcknowledged()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    MountInfo rootMount = testutil::makeMount(temp.path());
    MountInfo destMount = testutil::makeMount(destLibrary, QStringLiteral("ntfs3"), FsClass::Ntfs);
    const MountTable mounts = MountTable::fromMounts({rootMount, destMount});
    QString error;
    SteamScan scan = scanSteam({steamRoot, InstallKind::Native}, mounts, &error);
    QVERIFY(error.isEmpty());

    MoveOptions options;
    options.allowNtfsNative = false;
    const MovePlan unacknowledged = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY(unacknowledged.hasBlocker());
    bool ack = false;
    for (const Issue &issue : unacknowledged.issues) {
        if (issue.code == QStringLiteral("windows-filesystem-ack"))
            ack = true;
    }
    QVERIFY(ack);
    QVERIFY(hasWindowsFilesystemWarning(unacknowledged));

    options.allowNtfsNative = true;
    const MovePlan acknowledged = MovePlanner::plan(scan, 12345, destLibrary, options, mounts, &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!acknowledged.hasBlocker());
    Q_UNUSED(sourceLibrary);
}

QTEST_GUILESS_MAIN(MoverTest)
#include "tst_mover.moc"
