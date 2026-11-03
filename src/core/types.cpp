// SPDX-License-Identifier: GPL-3.0-or-later
#include "types.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <sys/stat.h>

namespace slm {

bool MountInfo::hasOption(const QString &name) const
{
    return options.contains(name, Qt::CaseInsensitive);
}

bool MountInfo::isWindowsCompatible() const
{
    return fsClass == FsClass::Ntfs || fsClass == FsClass::Exfat || fsClass == FsClass::Fat32;
}

bool MountInfo::isNetwork() const
{
    return fsClass == FsClass::Network;
}

bool MountInfo::isPosix() const
{
    return fsClass == FsClass::Posix;
}

QString MountInfo::describe() const
{
    return QStringLiteral("%1 (%2, %3)").arg(mountPoint, fsType, readOnly ? QStringLiteral("read-only")
                                                                          : QStringLiteral("read-write"));
}

QString SteamRoot::kindName() const
{
    switch (kind) {
    case InstallKind::Native:
        return QStringLiteral("Native");
    case InstallKind::Flatpak:
        return QStringLiteral("Flatpak");
    case InstallKind::Snap:
        return QStringLiteral("Snap");
    }
    return QStringLiteral("unknown");
}

QString Library::steamappsPath() const
{
    return path + QStringLiteral("/steamapps");
}

QString Library::compatdataPath() const
{
    return steamappsPath() + QStringLiteral("/compatdata");
}

bool AppManifest::isFullyInstalled() const
{
    return (stateFlags & 4u) != 0;
}

bool AppManifest::hasNonIdleFlags() const
{
    return stateFlags != 4u;
}

QStringList AppManifest::stateFlagNames() const
{
    struct Flag {
        quint32 bit;
        const char *name;
    };
    static const Flag flags[] = {
        {1u, "Uninstalled"},
        {2u, "UpdateRequired"},
        {4u, "FullyInstalled"},
        {8u, "Encrypted"},
        {16u, "Locked"},
        {32u, "FilesMissing"},
        {64u, "AppRunning"},
        {128u, "FilesCorrupt"},
        {256u, "UpdateRunning"},
        {512u, "UpdatePaused"},
        {1024u, "UpdateStarted"},
        {2048u, "Uninstalling"},
        {4096u, "BackupRunning"},
        {8192u, "Reconfiguring"},
        {16384u, "Validating"},
        {32768u, "AddingFiles"},
        {65536u, "Preallocating"},
        {131072u, "Downloading"},
        {262144u, "Staging"},
        {524288u, "Committing"},
        {1048576u, "UpdateStopping"},
    };

    QStringList result;
    for (const Flag &flag : flags) {
        if (stateFlags & flag.bit)
            result << QString::fromLatin1(flag.name);
    }
    if (result.isEmpty())
        result << QStringLiteral("None");
    return result;
}

bool Game::hasBlocker() const
{
    for (const Issue &issue : issues) {
        if (issue.isBlocker())
            return true;
    }
    return false;
}

bool Game::isMovable() const
{
    return category == GameCategory::Game && !hasBlocker();
}

const Library *SteamScan::findLibrary(const QString &pathOrRawPath) const
{
    const QString canonical = canonicalOrAbsolute(pathOrRawPath);
    for (const Library &library : libraries) {
        if (library.path == canonical || library.rawPath == pathOrRawPath
            || library.rawPath == canonical) {
            return &library;
        }
    }
    return nullptr;
}

const Game *SteamScan::findGame(quint32 appId) const
{
    for (const Game &game : games) {
        if (game.appId == appId)
            return &game;
    }
    return nullptr;
}

bool MovePlan::hasBlocker() const
{
    for (const Issue &issue : issues) {
        if (issue.isBlocker())
            return true;
    }
    return false;
}

QVector<Issue> MovePlan::blockers() const
{
    QVector<Issue> result;
    for (const Issue &issue : issues) {
        if (issue.isBlocker())
            result.append(issue);
    }
    return result;
}

QVector<Issue> MovePlan::warnings() const
{
    QVector<Issue> result;
    for (const Issue &issue : issues) {
        if (!issue.isBlocker())
            result.append(issue);
    }
    return result;
}

const Component *MovePlan::findComponent(ComponentKind kind) const
{
    for (const Component &component : components) {
        if (component.kind == kind)
            return &component;
    }
    return nullptr;
}

QString phaseName(Phase phase)
{
    switch (phase) {
    case Phase::Planned:
        return QStringLiteral("Planned");
    case Phase::CopyingGame:
        return QStringLiteral("Copying game");
    case Phase::CopyingAux:
        return QStringLiteral("Copying auxiliary data");
    case Phase::Verifying:
        return QStringLiteral("Verifying");
    case Phase::Syncing:
        return QStringLiteral("Syncing to disk");
    case Phase::WritingManifest:
        return QStringLiteral("Writing manifest");
    case Phase::UpdatingVdf:
        return QStringLiteral("Updating library database");
    case Phase::RemovingSourceManifest:
        return QStringLiteral("Removing source manifest");
    case Phase::RemovingSourcePayload:
        return QStringLiteral("Removing source files");
    case Phase::Done:
        return QStringLiteral("Done");
    case Phase::Failed:
        return QStringLiteral("Failed");
    case Phase::Cancelled:
        return QStringLiteral("Cancelled");
    }
    return QStringLiteral("Unknown");
}

QString Component::kindName() const
{
    return componentKindName(kind);
}

QString componentKindName(ComponentKind kind)
{
    switch (kind) {
    case ComponentKind::Game:
        return QStringLiteral("Game files");
    case ComponentKind::Manifest:
        return QStringLiteral("App manifest");
    case ComponentKind::Prefix:
        return QStringLiteral("Proton prefix");
    case ComponentKind::ShaderCache:
        return QStringLiteral("Shader cache");
    case ComponentKind::WorkshopContent:
        return QStringLiteral("Workshop content");
    case ComponentKind::WorkshopManifest:
        return QStringLiteral("Workshop manifest");
    }
    return QStringLiteral("Unknown");
}

QString runtimeKindName(RuntimeKind kind)
{
    switch (kind) {
    case RuntimeKind::Proton:
        return QStringLiteral("Proton");
    case RuntimeKind::Native:
        return QStringLiteral("Native");
    case RuntimeKind::Unknown:
        return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

QString gameCategoryName(GameCategory category)
{
    switch (category) {
    case GameCategory::Game:
        return QStringLiteral("Game");
    case GameCategory::Tool:
        return QStringLiteral("Tool");
    case GameCategory::SystemComponent:
        return QStringLiteral("Steam component");
    case GameCategory::Unmanaged:
        return QStringLiteral("Unmanaged");
    case GameCategory::Broken:
        return QStringLiteral("Broken");
    }
    return QStringLiteral("Unknown");
}

QString formatBytes(quint64 bytes)
{
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value >= 100.0 ? 0 : 1).arg(units[unit]);
}

QString decodeMountInfoEscapes(const QByteArray &input)
{
    QByteArray output;
    output.reserve(input.size());
    for (int i = 0; i < input.size(); ++i) {
        if (input.at(i) == '\\' && i + 3 < input.size() && input.at(i + 1) >= '0'
            && input.at(i + 1) <= '7' && input.at(i + 2) >= '0' && input.at(i + 2) <= '7'
            && input.at(i + 3) >= '0' && input.at(i + 3) <= '7') {
            const int value = (input.at(i + 1) - '0') * 64 + (input.at(i + 2) - '0') * 8
                + (input.at(i + 3) - '0');
            output.append(static_cast<char>(value));
            i += 3;
        } else {
            output.append(input.at(i));
        }
    }
    return QString::fromUtf8(output);
}

QString canonicalOrAbsolute(const QString &path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo info(path);
    if (info.exists()) {
        const QString canonical = info.canonicalFilePath();
        if (!canonical.isEmpty())
            return canonical;
    }
    return QDir::cleanPath(info.absoluteFilePath());
}

bool pathExistsNoFollow(const QString &path)
{
    struct stat st {};
    const QByteArray encoded = QFile::encodeName(path);
    return ::lstat(encoded.constData(), &st) == 0;
}

bool pathIsWithin(const QString &candidate, const QString &parent)
{
    if (candidate.isEmpty() || parent.isEmpty())
        return false;
    QString c = QDir::cleanPath(candidate);
    QString p = QDir::cleanPath(parent);
    if (!c.startsWith(QLatin1Char('/')))
        c = QDir::cleanPath(QDir::currentPath() + QLatin1Char('/') + c);
    if (!p.startsWith(QLatin1Char('/')))
        p = QDir::cleanPath(QDir::currentPath() + QLatin1Char('/') + p);
    if (c == p)
        return true;
    if (!p.endsWith(QLatin1Char('/')))
        p += QLatin1Char('/');
    return c.startsWith(p);
}

QByteArray sha256File(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        if (error)
            *error = QStringLiteral("Cannot hash %1: %2").arg(path, file.errorString());
        return {};
    }
    return hash.result();
}

} // namespace slm
