// SPDX-License-Identifier: GPL-3.0-or-later
#include "doctor.h"

#include "fileops.h"
#include "library.h"
#include "mountinfo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cerrno>
#include <cstring>

#include <sys/stat.h>
#include <unistd.h>
#include <unistd.h>

namespace slm {

namespace {

QString errnoString()
{
    return QString::fromLocal8Bit(::strerror(errno));
}

} // namespace

QVector<Issue> NtfsDoctor::inspect(const SteamScan &scan)
{
    QVector<Issue> issues;
    for (const Library &library : scan.libraries) {
        if (!library.mount.isWindowsCompatible())
            continue;
        if (library.compatdata.state == CompatDataState::Missing) {
            issues.append({Severity::Warning, QStringLiteral("doctor-compatdata-missing"),
                           QStringLiteral("%1 has no compatdata symlink").arg(library.rawPath),
                           QStringLiteral("Proton prefixes created inside this library may hit NTFS "
                                          "filename errors. Fix with the NTFS Doctor.")});
        } else if (library.compatdata.state == CompatDataState::BrokenSymlink) {
            issues.append({Severity::Blocker, QStringLiteral("doctor-compatdata-broken"),
                           QStringLiteral("%1 has a broken compatdata symlink").arg(library.rawPath),
                           library.compatdata.error});
        } else if (library.compatdata.state == CompatDataState::Directory) {
            issues.append({Severity::Warning, QStringLiteral("doctor-compatdata-real"),
                           QStringLiteral("%1 stores compatdata on a Windows filesystem").arg(library.rawPath),
                           QStringLiteral("Move the existing prefixes to a Linux filesystem and replace "
                                          "the directory with a symlink; the Doctor does not migrate "
                                          "non-empty prefix directories automatically.")});
        } else if (library.compatdata.targetOnWindowsFs) {
            issues.append({Severity::Warning, QStringLiteral("doctor-compatdata-target"),
                           QStringLiteral("%1 compatdata points to a Windows filesystem").arg(library.rawPath),
                           library.compatdata.target});
        }
        if (!library.mount.hasOption(QStringLiteral("permissions"))
            && !library.mount.hasOption(QStringLiteral("showexec"))) {
            issues.append({Severity::Warning, QStringLiteral("doctor-exec-bits"),
                           QStringLiteral("%1 does not preserve executable bits").arg(library.rawPath),
                           QStringLiteral("Fine for most Proton games; native Linux games may not launch.")});
        }
        if (!library.writeAccess) {
            issues.append({Severity::Warning, QStringLiteral("doctor-not-writable"),
                           QStringLiteral("%1 is not writable").arg(library.rawPath),
                           QStringLiteral("Check mount uid/gid/umask before moving games.")});
        }
    }
    return issues;
}

QString NtfsDoctor::defaultCompatDataTarget(const SteamScan &scan)
{
    // Prefer the Steam root library's compatdata, which is where most setups
    // already keep their shared prefixes.
    for (const Library &library : scan.libraries) {
        if (library.path == scan.root.path && library.compatdata.state != CompatDataState::Missing
            && !library.compatdata.targetOnWindowsFs) {
            return library.compatdata.state == CompatDataState::Symlink
                ? library.compatdata.target
                : library.compatdata.path;
        }
    }
    for (const Library &library : scan.libraries) {
        if (!library.mount.isWindowsCompatible()
            && library.compatdata.state != CompatDataState::Missing) {
            return library.compatdata.state == CompatDataState::Symlink
                ? library.compatdata.target
                : library.compatdata.path;
        }
    }
    return scan.root.path + QStringLiteral("/steamapps/compatdata");
}

bool NtfsDoctor::fixCompatData(const Library &library, const QString &targetPath,
                               const MountTable &mounts, bool apply, QString *error)
{
    if (::geteuid() == 0) {
        if (error)
            *error = QStringLiteral("Refusing to modify Steam data as root; run as the library owner.");
        return false;
    }
    const QString compatPath = library.compatdataPath();
    const QString target = QDir::cleanPath(targetPath);
    if (target.isEmpty()) {
        if (error)
            *error = QStringLiteral("No compatdata target was selected");
        return false;
    }
    // Compare the literal paths first. canonicalOrAbsolute() follows the
    // compatdata symlink, which made a perfectly valid "same target" request
    // look like "the target is the compatdata path itself".
    if (QDir::cleanPath(target) == QDir::cleanPath(compatPath)) {
        if (error)
            *error = QStringLiteral("The target must not be the compatdata path itself");
        return false;
    }
    if (library.compatdata.state == CompatDataState::Symlink
        && !library.compatdata.targetOnWindowsFs
        && canonicalOrAbsolute(library.compatdata.target) == canonicalOrAbsolute(target)) {
        // Already exactly what the user asked for. Recreating the link would
        // only add risk and makes the Doctor look like it did not detect the
        // existing setup.
        if (error)
            error->clear();
        return true;
    }
    if (library.compatdata.state != CompatDataState::Symlink
        && canonicalOrAbsolute(target) == canonicalOrAbsolute(compatPath)) {
        if (error)
            *error = QStringLiteral("The target must not be the compatdata path itself");
        return false;
    }
    const MountInfo targetMount = mounts.forPath(target);
    if (targetMount.isWindowsCompatible()) {
        if (error)
            *error = QStringLiteral("The compatdata target must be on a Linux filesystem, not %1")
                         .arg(targetMount.fsType);
        return false;
    }

    QString stateError;
    if (library.compatdata.state == CompatDataState::Symlink
        && canonicalOrAbsolute(library.compatdata.target) != canonicalOrAbsolute(target)) {
        const QDir oldDir(library.compatdata.target);
        const QStringList oldEntries = oldDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        if (!oldEntries.isEmpty()) {
            if (error)
                *error = QStringLiteral("The existing compatdata symlink points to %1, which contains "
                                        "prefix data. Refusing to re-point it and orphan those prefixes.")
                             .arg(library.compatdata.target);
            return false;
        }
    }

    if (library.compatdata.state == CompatDataState::BrokenSymlink
        || library.compatdata.state == CompatDataState::Missing
        || library.compatdata.state == CompatDataState::Symlink) {
        if (apply) {
            if (!fsutil::ensureDir(target, &stateError)) {
                if (error)
                    *error = stateError;
                return false;
            }
            if (library.compatdata.state == CompatDataState::Symlink) {
                if (!QFile::remove(compatPath)) {
                    if (error)
                        *error = QStringLiteral("Cannot remove old compatdata symlink %1").arg(compatPath);
                    return false;
                }
            }
            if (library.compatdata.state == CompatDataState::BrokenSymlink) {
                if (!QFile::remove(compatPath)) {
                    if (error)
                        *error = QStringLiteral("Cannot remove broken compatdata symlink %1").arg(compatPath);
                    return false;
                }
            }
            if (::symlink(QFile::encodeName(target).constData(),
                          QFile::encodeName(compatPath).constData())
                != 0) {
                if (error)
                    *error = QStringLiteral("Cannot create symlink %1 -> %2: %3")
                                 .arg(compatPath, target, errnoString());
                return false;
            }
            fsutil::fsyncDir(QFileInfo(compatPath).absolutePath(), nullptr);
        }
        if (error)
            error->clear();
        return true;
    }

    if (library.compatdata.state == CompatDataState::Directory) {
        const QDir dir(compatPath);
        const QStringList entries = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        if (!entries.isEmpty()) {
            if (error)
                *error = QStringLiteral("compatdata is a non-empty real directory at %1. "
                                        "Move its prefixes to Linux storage manually before fixing.")
                             .arg(compatPath);
            return false;
        }
        if (apply) {
            if (!fsutil::ensureDir(target, &stateError)) {
                if (error)
                    *error = stateError;
                return false;
            }
            if (::rmdir(QFile::encodeName(compatPath).constData()) != 0) {
                if (error)
                    *error = QStringLiteral("Cannot remove empty compatdata directory %1: %2")
                                 .arg(compatPath, errnoString());
                return false;
            }
            if (::symlink(QFile::encodeName(target).constData(),
                          QFile::encodeName(compatPath).constData())
                != 0) {
                if (error)
                    *error = QStringLiteral("Cannot create symlink %1 -> %2: %3")
                                 .arg(compatPath, target, errnoString());
                return false;
            }
        }
        if (error)
            error->clear();
        return true;
    }

    if (error)
        *error = library.compatdata.error.isEmpty()
            ? QStringLiteral("Unsupported compatdata state")
            : library.compatdata.error;
    return false;
}

} // namespace slm
