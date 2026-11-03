// SPDX-License-Identifier: GPL-3.0-or-later
#include "filetree.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <cerrno>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace slm {

namespace {

QString errnoString()
{
    return QString::fromLocal8Bit(::strerror(errno));
}

bool containsInvalidWindowsChar(const QString &name)
{
    for (const QChar c : name) {
        const ushort u = c.unicode();
        if (u < 0x20)
            return true;
        switch (u) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            return true;
        default:
            break;
        }
    }
    return false;
}

QString reservedDeviceReason(const QString &name)
{
    const QString base = name.section(QLatin1Char('.'), 0, 0).toUpper();
    static const QSet<QString> reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9"),
    };
    if (reserved.contains(base))
        return QStringLiteral("'%1' is a reserved Windows device name").arg(name);
    return {};
}

bool scanDirectory(const QString &absolutePath, const QString &relativePath, FileTree &tree,
                   QSet<QString> &seenInodes, QString *error)
{
    DIR *dir = ::opendir(QFile::encodeName(absolutePath).constData());
    if (!dir) {
        if (error)
            *error = QStringLiteral("Cannot open %1: %2").arg(absolutePath, errnoString());
        return false;
    }

    bool ok = true;
    while (dirent *entry = ::readdir(dir)) {
        const QByteArray rawName(entry->d_name);
        if (rawName == "." || rawName == "..")
            continue;

        const QString name = QFile::decodeName(rawName);
        const QString childAbsolute = absolutePath + QLatin1Char('/') + name;
        const QString childRelative =
            relativePath.isEmpty() ? name : relativePath + QLatin1Char('/') + name;

        struct stat st {};
        if (::lstat(QFile::encodeName(childAbsolute).constData(), &st) != 0) {
            if (error)
                *error = QStringLiteral("Cannot inspect %1: %2").arg(childAbsolute, errnoString());
            ok = false;
            break;
        }

        FileEntry fileEntry;
        fileEntry.relativePath = childRelative;
        fileEntry.mtimeSec = st.st_mtim.tv_sec;
        fileEntry.mtimeNsec = st.st_mtim.tv_nsec;
        fileEntry.mode = static_cast<quint32>(st.st_mode & 07777);
        fileEntry.device = static_cast<quint64>(st.st_dev);
        fileEntry.inode = static_cast<quint64>(st.st_ino);
        fileEntry.linkCount = static_cast<quint64>(st.st_nlink);

        if (S_ISLNK(st.st_mode)) {
            fileEntry.type = FileEntryType::Symlink;
            const QByteArray target = QFile::symLinkTarget(childAbsolute).toUtf8();
            fileEntry.symlinkTarget = QString::fromUtf8(target);
            fileEntry.size = 0;
            tree.symlinkCount++;
            tree.entries.append(fileEntry);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            fileEntry.type = FileEntryType::Directory;
            tree.directoryCount++;
            tree.entries.append(fileEntry);
            if (!scanDirectory(childAbsolute, childRelative, tree, seenInodes, error)) {
                ok = false;
                break;
            }
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            fileEntry.type = FileEntryType::File;
            fileEntry.size = static_cast<quint64>(st.st_size);
            tree.fileCount++;
            tree.logicalBytes += fileEntry.size;

            const QString inodeKey = QStringLiteral("%1:%2").arg(fileEntry.device).arg(fileEntry.inode);
            if (fileEntry.linkCount <= 1 || !seenInodes.contains(inodeKey)) {
                tree.uniqueBytes += fileEntry.size;
                if (fileEntry.linkCount > 1)
                    seenInodes.insert(inodeKey);
            }
            tree.entries.append(fileEntry);
            continue;
        }

        fileEntry.type = FileEntryType::Other;
        tree.entries.append(fileEntry);
    }
    ::closedir(dir);

    if (!ok && error && error->isEmpty())
        *error = QStringLiteral("Failed while scanning %1").arg(absolutePath);
    return ok;
}

} // namespace

FileTree FileTree::scan(const QString &rootPath)
{
    FileTree tree;
    tree.root = QDir::cleanPath(QFileInfo(rootPath).absoluteFilePath());

    struct stat st {};
    if (::lstat(QFile::encodeName(tree.root).constData(), &st) != 0) {
        tree.error = QStringLiteral("Cannot inspect %1: %2").arg(tree.root, errnoString());
        return tree;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        tree.error = QStringLiteral("Not a real directory (or is a symlink): %1").arg(tree.root);
        return tree;
    }

    QSet<QString> seenInodes;
    tree.ok = scanDirectory(tree.root, QString(), tree, seenInodes, &tree.error);
    return tree;
}

bool isValidWindowsComponent(const QString &name, QString *reason)
{
    auto reject = [&](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };

    if (name.isEmpty())
        return reject(QStringLiteral("empty filename"));
    if (name == QStringLiteral(".") || name == QStringLiteral(".."))
        return reject(QStringLiteral("'.' and '..' are not valid file names"));
    if (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        return reject(QStringLiteral("name ends with a dot or space: '%1'").arg(name));
    if (containsInvalidWindowsChar(name))
        return reject(QStringLiteral("name contains a Windows-invalid character: '%1'").arg(name));
    if (name.size() > 255)
        return reject(QStringLiteral("name is longer than 255 UTF-16 units: '%1'").arg(name));

    const QString reserved = reservedDeviceReason(name);
    if (!reserved.isEmpty())
        return reject(reserved);

    if (reason)
        reason->clear();
    return true;
}

void validateTreeForWindows(const FileTree &tree, bool fat32, QVector<Issue> *issues)
{
    if (!issues)
        return;
    struct Sibling
    {
        QString name;
        QString relativePath;
        FileEntryType type = FileEntryType::Other;
    };
    QHash<QString, QHash<QString, Sibling>> siblingsByParent;
    QSet<QString> reportedCollisions;
    int invalidCount = 0;
    int collisionCount = 0;

    for (const FileEntry &entry : tree.entries) {
        if (entry.relativePath.toUtf8().size() > 4000) {
            issues->append({Severity::Blocker, QStringLiteral("ntfs-long-path"),
                            QStringLiteral("A destination path is too long"),
                            QStringLiteral("%1 is longer than the safe 4096-byte path limit")
                                .arg(entry.relativePath.left(200))});
            continue;
        }
        const QStringList components = entry.relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QString parent;
        bool componentInvalid = false;
        for (const QString &component : components) {
            QString reason;
            if (!isValidWindowsComponent(component, &reason)) {
                if (invalidCount < 25) {
                    issues->append({Severity::Blocker, QStringLiteral("ntfs-invalid-name"),
                                    QStringLiteral("Destination is Windows-compatible but a filename is invalid"),
                                    QStringLiteral("%1: %2").arg(entry.relativePath, reason)});
                }
                invalidCount++;
                componentInvalid = true;
                break;
            }
            if (!parent.isEmpty())
                parent += QLatin1Char('/');
            parent += component;
        }
        if (componentInvalid)
            continue;

        if (components.isEmpty())
            continue;
        const QString name = components.last();
        const QString parentPath = components.size() > 1
            ? components.mid(0, components.size() - 1).join(QLatin1Char('/'))
            : QString();
        const QString folded = name.toCaseFolded();
        auto &siblings = siblingsByParent[parentPath];
        if (siblings.contains(folded) && siblings.value(folded).name != name) {
            const Sibling &first = siblings.value(folded);
            const QString key = parentPath + QLatin1Char('/') + folded;
            if (!reportedCollisions.contains(key) && collisionCount < 25) {
                QString detail = QStringLiteral("%1 and %2 (same directory: %3)")
                                     .arg(first.relativePath, entry.relativePath,
                                          parentPath.isEmpty() ? QStringLiteral(".")
                                                               : parentPath);
                if (first.type == FileEntryType::Symlink
                    || entry.type == FileEntryType::Symlink) {
                    detail += QStringLiteral(" — a case-only symlink cannot be represented on a "
                                             "case-insensitive filesystem.");
                }
                issues->append({Severity::Blocker, QStringLiteral("ntfs-case-collision"),
                                QStringLiteral("Filenames differ only by case"), detail});
                reportedCollisions.insert(key);
            }
            collisionCount++;
        } else {
            siblings.insert(folded, {name, entry.relativePath, entry.type});
        }

        if (fat32 && entry.type == FileEntryType::File && entry.size > 0xFFFFFFFFULL) {
            issues->append({Severity::Blocker, QStringLiteral("fat32-large-file"),
                            QStringLiteral("File is larger than the FAT32 limit"),
                            QStringLiteral("%1 is %2 bytes; FAT32 supports at most 4 GiB minus 1")
                                .arg(entry.relativePath)
                                .arg(entry.size)});
        }
    }

    if (invalidCount > 25) {
        issues->append({Severity::Blocker, QStringLiteral("ntfs-invalid-name-more"),
                        QStringLiteral("Additional invalid filenames were found"),
                        QStringLiteral("%1 more invalid names were not listed.").arg(invalidCount - 25)});
    }
    if (collisionCount > 25) {
        issues->append({Severity::Blocker, QStringLiteral("ntfs-case-collision-more"),
                        QStringLiteral("Additional case collisions were found"),
                        QStringLiteral("%1 more collisions were not listed.").arg(collisionCount - 25)});
    }
}

} // namespace slm
