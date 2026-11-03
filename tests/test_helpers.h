// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/mountinfo.h"
#include "core/types.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <unistd.h>

namespace testutil {

using slm::FsClass;
using slm::MountInfo;


inline bool writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

inline QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

inline QString manifestText(quint32 appId, const QString &name, const QString &installDir,
                            quint32 flags = 4, quint64 size = 1024, qint64 lastPlayed = 0,
                            quint64 buildId = 0)
{
    return QStringLiteral(
               "\"AppState\"\n"
               "{\n"
               "\t\"appid\"\t\t\"%1\"\n"
               "\t\"name\"\t\t\"%2\"\n"
               "\t\"StateFlags\"\t\t\"%3\"\n"
               "\t\"installdir\"\t\t\"%4\"\n"
               "\t\"SizeOnDisk\"\t\t\"%5\"\n"
               "\t\"LastPlayed\"\t\t\"%6\"\n"
               "\t\"buildid\"\t\t\"%7\"\n"
               "}\n")
        .arg(appId)
        .arg(name)
        .arg(flags)
        .arg(installDir)
        .arg(size)
        .arg(lastPlayed)
        .arg(buildId);
}

inline QString libraryFoldersText(const QString &root, const QVector<QPair<QString, QVector<quint32>>> &libraries)
{
    QString text = QStringLiteral("\"libraryfolders\"\n{\n");
    text += QStringLiteral("\t\"0\"\n\t{\n\t\t\"path\"\t\t\"%1\"\n\t\t\"label\"\t\t\"\"\n"
                           "\t\t\"apps\"\n\t\t{\n\t\t}\n\t}\n")
                .arg(root);
    int index = 1;
    for (const auto &library : libraries) {
        text += QStringLiteral("\t\"%1\"\n\t{\n\t\t\"path\"\t\t\"%2\"\n\t\t\"label\"\t\t\"\"\n"
                               "\t\t\"apps\"\n\t\t{\n")
                    .arg(index++)
                    .arg(library.first);
        for (quint32 appId : library.second)
            text += QStringLiteral("\t\t\t\"%1\"\t\t\"1024\"\n").arg(appId);
        text += QStringLiteral("\t\t}\n\t}\n");
    }
    text += QStringLiteral("}\n");
    return text;
}

inline MountInfo makeMount(const QString &mountPoint, const QString &fsType = QStringLiteral("ext4"),
                           FsClass fsClass = FsClass::Posix, quint64 free = 1000ull * 1024 * 1024 * 1024)
{
    MountInfo mount;
    mount.mountPoint = QDir::cleanPath(mountPoint);
    mount.fsType = fsType;
    mount.fsClass = fsClass;
    mount.options = {QStringLiteral("rw")};
    mount.freeBytes = free;
    mount.totalBytes = free * 2;
    mount.readOnly = false;
    mount.mountId = 1;
    mount.device = 1;
    return mount;
}

inline bool createSymlink(const QString &target, const QString &link)
{
    QDir().mkpath(QFileInfo(link).absolutePath());
    return ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(link).constData()) == 0;
}

inline QString makeFixtureSteam(QTemporaryDir &temp, bool withPrefix = true,
                                const QString &sourceFsType = QStringLiteral("ext4"),
                                const QString &destFsType = QStringLiteral("ext4"))
{
    const QString base = temp.path();
    const QString steamRoot = base + QStringLiteral("/steam");
    const QString source = base + QStringLiteral("/libraryA");
    const QString dest = base + QStringLiteral("/libraryB");
    QDir().mkpath(steamRoot + QStringLiteral("/steamapps"));
    QDir().mkpath(source + QStringLiteral("/steamapps/common/Test Game"));
    QDir().mkpath(dest + QStringLiteral("/steamapps"));
    QDir().mkpath(source + QStringLiteral("/steamapps/shadercache/12345"));
    QDir().mkpath(source + QStringLiteral("/steamapps/workshop/content/12345"));
    QDir().mkpath(dest + QStringLiteral("/steamapps/workshop/content"));

    writeFile(source + QStringLiteral("/steamapps/common/Test Game/game.dat"),
              QByteArray(1024 * 1024, 'A'));
    writeFile(source + QStringLiteral("/steamapps/common/Test Game/readme.txt"),
              QByteArray("hello\n"));
    writeFile(source + QStringLiteral("/steamapps/shadercache/12345/cache.bin"),
              QByteArray(4096, 'S'));
    writeFile(source + QStringLiteral("/steamapps/workshop/content/12345/mod.bin"),
              QByteArray(2048, 'M'));
    writeFile(source + QStringLiteral("/steamapps/appmanifest_12345.acf"),
              manifestText(12345, QStringLiteral("Test Game"), QStringLiteral("Test Game"), 4,
                           1024 * 1024 + 6).toUtf8());
    writeFile(source + QStringLiteral("/steamapps/workshop/appworkshop_12345.acf"),
              QByteArray("\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"12345\"\n}\n"));
    writeFile(steamRoot + QStringLiteral("/steamapps/libraryfolders.vdf"),
              libraryFoldersText(steamRoot, {{source, {12345}}, {dest, {}}}).toUtf8());
    writeFile(steamRoot + QStringLiteral("/appcache/librarycache/12345/header.jpg"),
              QByteArray::fromBase64("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"));

    if (withPrefix) {
        QDir().mkpath(steamRoot + QStringLiteral("/steamapps/compatdata/12345/pfx"));
        writeFile(steamRoot + QStringLiteral("/steamapps/compatdata/12345/pfx/system.reg"),
                  QByteArray("prefix"));
        createSymlink(steamRoot + QStringLiteral("/steamapps/compatdata"),
                      source + QStringLiteral("/steamapps/compatdata"));
        createSymlink(steamRoot + QStringLiteral("/steamapps/compatdata"),
                      dest + QStringLiteral("/steamapps/compatdata"));
    }
    Q_UNUSED(sourceFsType);
    Q_UNUSED(destFsType);
    return steamRoot;
}

} // namespace testutil
