// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/fileops.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

using namespace slm;

class FileOpsTest : public QObject
{
    Q_OBJECT
private slots:
    void removeTreeRemovesNestedTree();
    void removeTreeDoesNotFollowSymlinks();
    void removeTreeRemovesRootSymlinkOnly();
    void removeTreeRemovesRegularFile();
};

void FileOpsTest::removeTreeRemovesNestedTree()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = temp.path() + QStringLiteral("/root");
    QVERIFY(QDir().mkpath(root + QStringLiteral("/a/b")));
    QFile file(root + QStringLiteral("/a/b/data.bin"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("payload");
    file.close();

    QString error;
    QVERIFY2(fsutil::removeTree(root, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(root));
}

void FileOpsTest::removeTreeDoesNotFollowSymlinks()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = temp.path() + QStringLiteral("/root");
    const QString outside = temp.path() + QStringLiteral("/outside");
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QDir().mkpath(outside));
    QFile keep(outside + QStringLiteral("/keep.txt"));
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.write("do not delete");
    keep.close();
    QVERIFY(::symlink(QFile::encodeName(outside).constData(),
                      QFile::encodeName(root + QStringLiteral("/escape")).constData()) == 0);

    QString error;
    QVERIFY2(fsutil::removeTree(root, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(root));
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));
}

void FileOpsTest::removeTreeRemovesRootSymlinkOnly()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString outside = temp.path() + QStringLiteral("/outside");
    QVERIFY(QDir().mkpath(outside));
    QFile keep(outside + QStringLiteral("/keep.txt"));
    QVERIFY(keep.open(QIODevice::WriteOnly));
    keep.write("do not delete");
    keep.close();
    const QString link = temp.path() + QStringLiteral("/link");
    QVERIFY(::symlink(QFile::encodeName(outside).constData(),
                      QFile::encodeName(link).constData()) == 0);

    QString error;
    QVERIFY2(fsutil::removeTree(link, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(link));
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/keep.txt")));
}

void FileOpsTest::removeTreeRemovesRegularFile()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.path() + QStringLiteral("/file.txt");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("payload");
    file.close();

    QString error;
    QVERIFY2(fsutil::removeTree(path, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(path));
}

QTEST_MAIN(FileOpsTest)
#include "tst_fileops.moc"
