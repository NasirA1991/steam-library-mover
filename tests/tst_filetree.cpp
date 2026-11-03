// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/filetree.h"

#include "test_helpers.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

#include <unistd.h>

using namespace slm;

class FileTreeTest : public QObject
{
    Q_OBJECT
private slots:
    void scanAndCount();
    void windowsNames();
    void caseCollisions();
    void fat32Limit();
};

void FileTreeTest::scanAndCount()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(testutil::writeFile(temp.path() + QStringLiteral("/a.txt"), QByteArray(100, 'x')));
    QDir().mkpath(temp.path() + QStringLiteral("/sub"));
    QVERIFY(testutil::writeFile(temp.path() + QStringLiteral("/sub/b.bin"), QByteArray(200, 'y')));
    QVERIFY(::symlink("a.txt", (temp.path() + QStringLiteral("/link")).toLocal8Bit().constData()) == 0);
    QVERIFY(::link((temp.path() + QStringLiteral("/a.txt")).toLocal8Bit().constData(),
                   (temp.path() + QStringLiteral("/hard")).toLocal8Bit().constData()) == 0);

    const FileTree tree = FileTree::scan(temp.path());
    QVERIFY2(tree.ok, qPrintable(tree.error));
    QCOMPARE(tree.fileCount, 3); // a.txt, b.bin, hard
    QCOMPARE(tree.directoryCount, 1);
    QCOMPARE(tree.symlinkCount, 1);
    QCOMPARE(tree.logicalBytes, 400ull);
    QCOMPARE(tree.uniqueBytes, 300ull);
}

void FileTreeTest::windowsNames()
{
    QString reason;
    QVERIFY(isValidWindowsComponent(QStringLiteral("normal.exe"), &reason));
    QVERIFY(!isValidWindowsComponent(QStringLiteral("bad:name"), &reason));
    QVERIFY(!isValidWindowsComponent(QStringLiteral("bad\\name"), &reason));
    QVERIFY(!isValidWindowsComponent(QStringLiteral("CON"), &reason));
    QVERIFY(!isValidWindowsComponent(QStringLiteral("con.txt"), &reason));
    QVERIFY(!isValidWindowsComponent(QStringLiteral("trailing."), &reason));
    QVERIFY(!isValidWindowsComponent(QStringLiteral("trailing "), &reason));
    QVERIFY(!isValidWindowsComponent(QString(), &reason));
    QVERIFY(!isValidWindowsComponent(QString(300, QLatin1Char('a')), &reason));
}

void FileTreeTest::caseCollisions()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(testutil::writeFile(temp.path() + QStringLiteral("/File.txt"), "a"));
    QVERIFY(testutil::writeFile(temp.path() + QStringLiteral("/file.txt"), "b"));
    const FileTree tree = FileTree::scan(temp.path());
    QVERIFY(tree.ok);
    QVector<Issue> issues;
    validateTreeForWindows(tree, false, &issues);
    bool found = false;
    for (const Issue &issue : issues) {
        if (issue.code == QStringLiteral("ntfs-case-collision")) {
            found = true;
            QVERIFY(issue.detail.contains(QStringLiteral("File.txt")));
            QVERIFY(issue.detail.contains(QStringLiteral("file.txt")));
            QVERIFY(issue.detail.contains(QStringLiteral("same directory")));
        }
    }
    QVERIFY(found);
}

void FileTreeTest::fat32Limit()
{
    FileTree tree;
    tree.ok = true;
    FileEntry entry;
    entry.relativePath = QStringLiteral("big.bin");
    entry.type = FileEntryType::File;
    entry.size = 5ull * 1024 * 1024 * 1024;
    tree.entries.append(entry);
    QVector<Issue> issues;
    validateTreeForWindows(tree, true, &issues);
    bool found = false;
    for (const Issue &issue : issues) {
        if (issue.code == QStringLiteral("fat32-large-file"))
            found = true;
    }
    QVERIFY(found);
}

QTEST_GUILESS_MAIN(FileTreeTest)
#include "tst_filetree.moc"
