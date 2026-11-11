// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/doctor.h"
#include "core/process_check.h"
#include "core/library.h"
#include "core/mountinfo.h"

#include "test_helpers.h"

#include <QDir>
#include <QFile>
#include <sys/stat.h>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

using namespace slm;

class DoctorTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void createsMissingSymlink();
    void replacesEmptyDirectory();
    void refusesNonEmptyDirectory();
    void refusesWindowsTarget();
    void sameTargetIsNoOp();
};

static Library makeLibrary(const QString &path)
{
    Library library;
    library.path = QDir::cleanPath(path);
    library.rawPath = library.path;
    library.mount = testutil::makeMount(path);
    library.compatdata = inspectCompatData(library, MountTable::fromMounts({library.mount}));
    return library;
}

void DoctorTest::initTestCase()
{
    if (::geteuid() == 0)
        QSKIP("Doctor write tests must run as a normal user");
    slm::setProcessInspectorForTesting([](const QStringList &, const QStringList &) {
        return slm::ProcessUsage{};
    });
}

void DoctorTest::createsMissingSymlink()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString libraryPath = temp.path() + QStringLiteral("/lib");
    QDir().mkpath(libraryPath + QStringLiteral("/steamapps"));
    Library library = makeLibrary(libraryPath);
    const QString target = temp.path() + QStringLiteral("/prefixes");
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    QVERIFY2(NtfsDoctor::fixCompatData(library, target, mounts, true, &error), qPrintable(error));
    QVERIFY(QFileInfo(library.compatdataPath()).isSymLink());
    QCOMPARE(QFileInfo(library.compatdataPath()).symLinkTarget(), target);
}

void DoctorTest::replacesEmptyDirectory()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString libraryPath = temp.path() + QStringLiteral("/lib");
    QDir().mkpath(libraryPath + QStringLiteral("/steamapps/compatdata"));
    Library library = makeLibrary(libraryPath);
    QCOMPARE(library.compatdata.state, CompatDataState::Directory);
    const QString target = temp.path() + QStringLiteral("/prefixes");
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    QVERIFY2(NtfsDoctor::fixCompatData(library, target, mounts, true, &error), qPrintable(error));
    QVERIFY(QFileInfo(library.compatdataPath()).isSymLink());
}

void DoctorTest::refusesNonEmptyDirectory()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString libraryPath = temp.path() + QStringLiteral("/lib");
    QDir().mkpath(libraryPath + QStringLiteral("/steamapps/compatdata/12345"));
    Library library = makeLibrary(libraryPath);
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    QVERIFY(!NtfsDoctor::fixCompatData(library, temp.path() + QStringLiteral("/prefixes"), mounts,
                                       true, &error));
    QVERIFY(error.contains(QStringLiteral("non-empty")));
}

void DoctorTest::refusesWindowsTarget()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString libraryPath = temp.path() + QStringLiteral("/lib");
    QDir().mkpath(libraryPath + QStringLiteral("/steamapps"));
    Library library = makeLibrary(libraryPath);
    MountInfo windows = testutil::makeMount(temp.path() + QStringLiteral("/ntfs"),
                                            QStringLiteral("ntfs3"), FsClass::Ntfs);
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path()), windows});
    QString error;
    QVERIFY(!NtfsDoctor::fixCompatData(library, temp.path() + QStringLiteral("/ntfs/prefixes"),
                                       mounts, true, &error));
    QVERIFY(error.contains(QStringLiteral("Linux filesystem")));
}

void DoctorTest::sameTargetIsNoOp()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString libraryPath = temp.path() + QStringLiteral("/lib");
    const QString target = temp.path() + QStringLiteral("/prefixes");
    QDir().mkpath(libraryPath + QStringLiteral("/steamapps"));
    QDir().mkpath(target);
    QVERIFY(testutil::createSymlink(target, libraryPath + QStringLiteral("/steamapps/compatdata")));
    Library library = makeLibrary(libraryPath);
    QCOMPARE(library.compatdata.state, CompatDataState::Symlink);
    struct stat before {};
    QCOMPARE(::lstat(QFile::encodeName(library.compatdataPath()).constData(), &before), 0);
    const MountTable mounts = MountTable::fromMounts({testutil::makeMount(temp.path())});
    QString error;
    QVERIFY2(NtfsDoctor::fixCompatData(library, target, mounts, true, &error), qPrintable(error));
    struct stat after {};
    QCOMPARE(::lstat(QFile::encodeName(library.compatdataPath()).constData(), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(QFileInfo(library.compatdataPath()).symLinkTarget(), target);
}

QTEST_GUILESS_MAIN(DoctorTest)
#include "tst_doctor.moc"
