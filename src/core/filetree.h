// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "types.h"

#include <QString>
#include <QVector>

namespace slm {

enum class FileEntryType { File, Directory, Symlink, Other };

struct FileEntry
{
    QString relativePath;
    FileEntryType type = FileEntryType::Other;
    quint64 size = 0;
    quint32 mode = 0;
    qint64 mtimeSec = 0;
    qint64 mtimeNsec = 0;
    quint64 device = 0;
    quint64 inode = 0;
    quint64 linkCount = 0;
    QString symlinkTarget;
};

struct FileTree
{
    QString root;
    QVector<FileEntry> entries;
    quint64 logicalBytes = 0;
    quint64 uniqueBytes = 0;
    int fileCount = 0;
    int directoryCount = 0;
    int symlinkCount = 0;

    bool ok = false;
    QString error;

    static FileTree scan(const QString &rootPath);
};

bool isValidWindowsComponent(const QString &name, QString *reason = nullptr);
void validateTreeForWindows(const FileTree &tree, bool fat32, QVector<Issue> *issues);

} // namespace slm
