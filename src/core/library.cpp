// SPDX-License-Identifier: GPL-3.0-or-later
#include "library.h"

#include "appmanifest.h"
#include "fileops.h"

#include <QCryptographicHash>
#include <QDir>
#include <QSet>
#include <QFile>
#include <QFileInfo>

#include <cerrno>

#include <sys/stat.h>
#include <unistd.h>

namespace slm {

namespace {

constexpr qint64 kMaxVdfSize = 16 * 1024 * 1024;

QString joinPath(const QString &base, const QString &name)
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

bool directoryExists(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() && info.isDir();
}

bool writable(const QString &path)
{
    const QByteArray encoded = QFile::encodeName(path);
    return ::access(encoded.constData(), W_OK) == 0;
}

} // namespace

LibraryLoadResult loadLibraries(const SteamRoot &root, const MountTable &mounts)
{
    LibraryLoadResult result;
    result.vdfPath = joinPath(root.path, QStringLiteral("steamapps/libraryfolders.vdf"));

    QFile file(result.vdfPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot read %1: %2").arg(result.vdfPath, file.errorString());
        return result;
    }
    if (file.size() > kMaxVdfSize) {
        result.error = QStringLiteral("%1 is unexpectedly large").arg(result.vdfPath);
        return result;
    }
    result.vdfRaw = file.readAll();
    file.close();

    QString parseError;
    int offset = 0;
    const auto document = VdfDocument::parse(result.vdfRaw, &parseError, &offset);
    if (!document) {
        result.error = QStringLiteral("Invalid library database %1 at byte %2: %3")
                           .arg(result.vdfPath)
                           .arg(offset)
                           .arg(parseError);
        return result;
    }

    const VdfNode *librariesObject = document->object(QStringLiteral("libraryfolders"));
    if (!librariesObject) {
        result.error = QStringLiteral("No 'libraryfolders' object in %1").arg(result.vdfPath);
        return result;
    }

    QSet<QString> seenPaths;
    bool sawLegacyFormat = false;

    for (const VdfNode &entry : librariesObject->children) {
        Library library;
        library.rootPath = root.path;
        library.vdfKey = entry.key;

        if (!entry.isObject) {
            sawLegacyFormat = true;
            library.rawPath = entry.value;
            library.registered = true;
            result.issues.append({Severity::Blocker, QStringLiteral("legacy-vdf"),
                                  QStringLiteral("Legacy Steam library database format"),
                                  QStringLiteral("The library database is in the old format; "
                                                 "start Steam once to upgrade it before moving games.")});
        } else {
            library.rawPath = entry.childValue(QStringLiteral("path"));
            library.label = entry.childValue(QStringLiteral("label"));
            const VdfNode *apps = entry.object(QStringLiteral("apps"));
            if (apps) {
                for (const VdfNode &app : apps->children) {
                    if (app.isObject)
                        continue;
                    bool idOk = false;
                    const quint32 appId = app.key.toUInt(&idOk, 10);
                    if (!idOk || appId == 0)
                        continue;
                    bool sizeOk = false;
                    const quint64 size = app.value.toULongLong(&sizeOk, 10);
                    library.apps.insert(appId, sizeOk ? size : 0);
                }
            }
        }

        if (library.rawPath.trimmed().isEmpty()) {
            result.issues.append({Severity::Warning, QStringLiteral("empty-library-path"),
                                  QStringLiteral("Ignoring library entry with an empty path"),
                                  QStringLiteral("VDF key %1").arg(library.vdfKey)});
            continue;
        }

        library.path = canonicalOrAbsolute(library.rawPath);
        if (seenPaths.contains(library.path))
            continue;
        seenPaths.insert(library.path);

        if (!directoryExists(library.path)) {
            result.issues.append({Severity::Warning, QStringLiteral("library-missing"),
                                  QStringLiteral("Library path is not available: %1").arg(library.rawPath),
                                  QStringLiteral("Mount the drive, then rescan.")});
        }
        if (!directoryExists(joinPath(library.path, QStringLiteral("steamapps")))) {
            result.issues.append({Severity::Warning, QStringLiteral("steamapps-missing"),
                                  QStringLiteral("Not a Steam library: %1").arg(library.rawPath),
                                  QStringLiteral("The steamapps directory is missing.")});
        }

        library.mount = mounts.forPath(library.path);
        library.writeAccess = writable(joinPath(library.path, QStringLiteral("steamapps")));
        library.compatdata = inspectCompatData(library, mounts);
        result.libraries.append(library);
    }

    // Some valid setups omit the Steam root from libraryfolders.vdf; add it as a fallback.
    const QString rootLibraryPath = canonicalOrAbsolute(root.path);
    if (!seenPaths.contains(rootLibraryPath)) {
        Library rootLibrary;
        rootLibrary.rawPath = root.path;
        rootLibrary.path = rootLibraryPath;
        rootLibrary.rootPath = root.path;
        rootLibrary.vdfKey = QStringLiteral("__root");
        rootLibrary.mount = mounts.forPath(rootLibrary.path);
        rootLibrary.writeAccess = writable(joinPath(rootLibrary.path, QStringLiteral("steamapps")));
        rootLibrary.compatdata = inspectCompatData(rootLibrary, mounts);
        result.libraries.append(rootLibrary);
    }

    result.ok = true;
    if (sawLegacyFormat) {
        // Already recorded above.
    }
    return result;
}

CompatDataInfo inspectCompatData(const Library &library, const MountTable &mounts)
{
    CompatDataInfo info;
    info.path = joinPath(library.path, QStringLiteral("steamapps/compatdata"));

    struct stat st {};
    const QByteArray encoded = QFile::encodeName(info.path);
    if (::lstat(encoded.constData(), &st) != 0) {
        if (errno == ENOENT) {
            info.state = CompatDataState::Missing;
        } else {
            info.state = CompatDataState::Missing;
            info.error = QStringLiteral("Cannot inspect %1").arg(info.path);
        }
        return info;
    }

    if (S_ISLNK(st.st_mode)) {
        info.state = CompatDataState::Symlink;
        const auto target = fsutil::readSymlinkTarget(info.path);
        info.linkTarget = target ? QString::fromUtf8(*target) : QString();
        const QFileInfo targetInfo(info.path);
        const QString canonical = targetInfo.canonicalFilePath();
        if (canonical.isEmpty()) {
            info.state = CompatDataState::BrokenSymlink;
            info.error = QStringLiteral("The compatdata symlink is broken: %1 -> %2")
                             .arg(info.path, info.linkTarget);
            return info;
        }
        info.target = canonical;
        const MountInfo mount = mounts.forPath(info.target);
        info.targetOnWindowsFs = mount.isWindowsCompatible();
        if (!targetInfo.isDir()) {
            info.state = CompatDataState::BrokenSymlink;
            info.error = QStringLiteral("The compatdata symlink target is not a directory: %1")
                             .arg(info.target);
        }
        return info;
    }

    if (S_ISDIR(st.st_mode)) {
        info.state = CompatDataState::Directory;
        info.target = info.path;
        const MountInfo mount = mounts.forPath(info.path);
        info.targetOnWindowsFs = mount.isWindowsCompatible();
        return info;
    }

    info.state = CompatDataState::Missing;
    info.error = QStringLiteral("compatdata exists but is not a directory or symlink: %1").arg(info.path);
    return info;
}

QString resolvePrefixPath(const Library &library, quint32 appId, bool *exists)
{
    const CompatDataInfo &info = library.compatdata;
    QString prefix;
    if (info.state == CompatDataState::Symlink) {
        if (info.target.isEmpty()) {
            if (exists)
                *exists = false;
            return {};
        }
        prefix = joinPath(info.target, QString::number(appId));
    } else {
        prefix = joinPath(info.path, QString::number(appId));
    }

    const QFileInfo prefixInfo(prefix);
    if (exists)
        *exists = prefixInfo.exists();
    return QDir::cleanPath(prefixInfo.absoluteFilePath());
}

bool LibraryFoldersFile::load(const QString &path, QString *error)
{
    m_path = path;
    m_loaded = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.size() > kMaxVdfSize) {
        if (error)
            *error = QStringLiteral("%1 is unexpectedly large").arg(path);
        return false;
    }
    m_raw = file.readAll();
    const auto document = VdfDocument::parse(m_raw, error);
    if (!document)
        return false;
    m_document = *document;
    m_loaded = true;
    return true;
}

bool LibraryFoldersFile::reload(QString *error)
{
    return load(m_path, error);
}

QByteArray LibraryFoldersFile::raw() const
{
    return m_document.serialize();
}

QByteArray LibraryFoldersFile::hash() const
{
    return QCryptographicHash::hash(m_raw, QCryptographicHash::Sha256);
}

bool LibraryFoldersFile::pathMatches(const VdfNode &object, const QString &path)
{
    if (!object.isObject)
        return false;
    const QString stored = object.childValue(QStringLiteral("path"));
    if (stored.isEmpty())
        return false;
    return stored == path || canonicalOrAbsolute(stored) == canonicalOrAbsolute(path);
}

VdfNode *LibraryFoldersFile::libraryObject(const QString &path)
{
    VdfNode *root = m_document.object(QStringLiteral("libraryfolders"));
    if (!root)
        return nullptr;
    for (VdfNode &child : root->children) {
        if (pathMatches(child, path))
            return &child;
    }
    return nullptr;
}

bool LibraryFoldersFile::moveApp(quint32 appId, const QString &sourcePath, const QString &destPath,
                                 quint64 size, QString *error)
{
    if (!m_loaded) {
        if (error)
            *error = QStringLiteral("Library database is not loaded");
        return false;
    }
    VdfNode *source = libraryObject(sourcePath);
    VdfNode *dest = libraryObject(destPath);
    if (!dest) {
        if (error)
            *error = QStringLiteral("Destination library is not registered in %1: %2")
                         .arg(m_path, destPath);
        return false;
    }
    if (!source) {
        if (error)
            *error = QStringLiteral("Source library is not registered in %1: %2")
                         .arg(m_path, sourcePath);
        return false;
    }

    auto updateApps = [&](VdfNode *library, bool add) -> bool {
        VdfNode *apps = library->object(QStringLiteral("apps"));
        if (!apps) {
            VdfNode node;
            node.key = QStringLiteral("apps");
            node.isObject = true;
            library->children.append(node);
            apps = &library->children.last();
        }
        const QString key = QString::number(appId);
        for (int i = 0; i < apps->children.size(); ++i) {
            if (apps->children.at(i).key == key) {
                if (add) {
                    apps->children[i].value = QString::number(size);
                    apps->children[i].isObject = false;
                    apps->children[i].children.clear();
                } else {
                    apps->children.removeAt(i);
                }
                return true;
            }
        }
        if (add) {
            VdfNode node;
            node.key = key;
            node.value = QString::number(size);
            node.isObject = false;
            apps->children.append(node);
        }
        return true;
    };

    // Remove from every library first to heal stale duplicate entries.
    if (VdfNode *root = m_document.object(QStringLiteral("libraryfolders"))) {
        for (VdfNode &child : root->children) {
            if (!child.isObject)
                continue;
            VdfNode *apps = child.object(QStringLiteral("apps"));
            if (!apps)
                continue;
            const QString key = QString::number(appId);
            for (int i = apps->children.size() - 1; i >= 0; --i) {
                if (apps->children.at(i).key == key)
                    apps->children.removeAt(i);
            }
        }
    }
    if (!updateApps(dest, true)) {
        if (error)
            *error = QStringLiteral("Cannot update destination apps map");
        return false;
    }
    Q_UNUSED(source);
    return true;
}

bool LibraryFoldersFile::save(QString *error) const
{
    if (!m_loaded) {
        if (error)
            *error = QStringLiteral("Library database is not loaded");
        return false;
    }
    return fsutil::writeFileAtomic(m_path, m_document.serialize(), 0644, error);
}

} // namespace slm
