// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/mountinfo.h"

#include "test_helpers.h"

#include <QtTest>

using namespace slm;

class MountInfoTest : public QObject
{
    Q_OBJECT
private slots:
    void decodeEscapes();
    void classify();
    void parseMountInfo();
    void longestPrefix();
};

void MountInfoTest::decodeEscapes()
{
    QCOMPARE(decodeMountInfoEscapes("/mnt/My\\040Games"), QStringLiteral("/mnt/My Games"));
    QCOMPARE(decodeMountInfoEscapes("/a\\011b\\134c"), QStringLiteral("/a\tb\\c"));
}

void MountInfoTest::classify()
{
    QCOMPARE(classifyFilesystem(QStringLiteral("ext4")), FsClass::Posix);
    QCOMPARE(classifyFilesystem(QStringLiteral("btrfs")), FsClass::Posix);
    QCOMPARE(classifyFilesystem(QStringLiteral("ntfs3")), FsClass::Ntfs);
    QCOMPARE(classifyFilesystem(QStringLiteral("ntfs")), FsClass::Ntfs);
    QCOMPARE(classifyFilesystem(QStringLiteral("fuseblk")), FsClass::Ntfs);
    QCOMPARE(classifyFilesystem(QStringLiteral("exfat")), FsClass::Exfat);
    QCOMPARE(classifyFilesystem(QStringLiteral("vfat")), FsClass::Fat32);
    QCOMPARE(classifyFilesystem(QStringLiteral("nfs4")), FsClass::Network);
    QVERIFY(isWindowsCompatibleClass(FsClass::Ntfs));
    QVERIFY(!isWindowsCompatibleClass(FsClass::Posix));
}

void MountInfoTest::parseMountInfo()
{
    const QByteArray data = R"(36 35 98:0 /mnt1 /mnt/My\040Games rw,noatime master:1 - ntfs3 /dev/sda1 rw,uid=1000
37 36 0:32 / /home rw,relatime - btrfs /dev/sda2 rw,compress=zstd
)";
    QString error;
    const QVector<MountInfo> mounts = MountTable::parseMountInfo(data, &error);
    QCOMPARE(mounts.size(), 2);
    bool foundGames = false;
    bool foundHome = false;
    for (const MountInfo &mount : mounts) {
        if (mount.mountPoint == QStringLiteral("/mnt/My Games")) {
            foundGames = true;
            QCOMPARE(mount.fsType, QStringLiteral("ntfs3"));
            QCOMPARE(mount.fsClass, FsClass::Ntfs);
            QVERIFY(mount.options.contains(QStringLiteral("rw")));
        }
        if (mount.mountPoint == QStringLiteral("/home")) {
            foundHome = true;
            QCOMPARE(mount.fsClass, FsClass::Posix);
        }
    }
    QVERIFY(foundGames);
    QVERIFY(foundHome);
}

void MountInfoTest::longestPrefix()
{
    MountInfo root = testutil::makeMount(QStringLiteral("/"));
    MountInfo tmpfs = testutil::makeMount(QStringLiteral("/mnt/games"));
    MountInfo nested = testutil::makeMount(QStringLiteral("/mnt/games/ntfs"));
    nested.fsType = QStringLiteral("ntfs3");
    nested.fsClass = FsClass::Ntfs;
    const MountTable table = MountTable::fromMounts({root, tmpfs, nested});
    QCOMPARE(table.forPath(QStringLiteral("/mnt/games/x")).mountPoint, QStringLiteral("/mnt/games"));
    QCOMPARE(table.forPath(QStringLiteral("/mnt/games/ntfs/Library")).mountPoint,
             QStringLiteral("/mnt/games/ntfs"));
    QCOMPARE(table.forPath(QStringLiteral("/elsewhere")).mountPoint, QStringLiteral("/"));
}

QTEST_GUILESS_MAIN(MountInfoTest)
#include "tst_mountinfo.moc"
