// SPDX-License-Identifier: GPL-3.0-or-later
#include "scanner.h"

#include "appmanifest.h"
#include "library.h"
#include "mountinfo.h"
#include "types.h"
#include "vdf.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <functional>

#include <sys/stat.h>

namespace slm {

namespace {

QString joinPath(const QString &base, const QString &name)
{
    if (base.endsWith(QLatin1Char('/')))
        return base + name;
    return base + QLatin1Char('/') + name;
}

QString homeDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
}

InstallKind kindForPath(const QString &path)
{
    if (path.contains(QStringLiteral("/.var/app/com.valvesoftware.Steam/")))
        return InstallKind::Flatpak;
    if (path.contains(QStringLiteral("/snap/steam/")))
        return InstallKind::Snap;
    return InstallKind::Native;
}

const VdfNode *findCompatMapping(const VdfNode &node)
{
    if (node.isObject && node.key == QStringLiteral("CompatToolMapping"))
        return &node;
    for (const VdfNode &child : node.children) {
        if (const VdfNode *found = findCompatMapping(child))
            return found;
    }
    return nullptr;
}

void mergeCompatMapping(const QString &path, QMap<quint32, QString> *result)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024)
        return;
    const auto document = VdfDocument::parse(file.readAll());
    if (!document)
        return;
    const VdfNode *mapping = findCompatMapping(document->root);
    if (!mapping)
        return;
    for (const VdfNode &entry : mapping->children) {
        if (!entry.isObject)
            continue;
        bool ok = false;
        const quint32 appId = entry.key.toUInt(&ok, 10);
        if (!ok || appId == 0)
            continue;
        const QString name = entry.childValue(QStringLiteral("name"));
        if (name.isEmpty())
            result->remove(appId);
        else
            result->insert(appId, name);
    }
}

QMap<quint32, QString> loadCompatToolMapping(const SteamRoot &root)
{
    QMap<quint32, QString> result;
    mergeCompatMapping(root.path + QStringLiteral("/config/config.vdf"), &result);
    QDir userdata(root.path + QStringLiteral("/userdata"));
    const QStringList users = userdata.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &user : users) {
        mergeCompatMapping(userdata.filePath(user) + QStringLiteral("/config/localconfig.vdf"),
                           &result);
    }
    return result;
}

QString prettyProtonName(const QString &raw)
{
    if (raw.isEmpty())
        return {};
    const QString lower = raw.toLower();
    if (lower == QStringLiteral("proton_experimental"))
        return QStringLiteral("Proton Experimental");
    if (lower == QStringLiteral("proton_hotfix"))
        return QStringLiteral("Proton Hotfix");
    static const QRegularExpression numbered(QStringLiteral(R"(^proton[_-]?([0-9]+)$)"));
    const QRegularExpressionMatch match = numbered.match(lower);
    if (match.hasMatch())
        return QStringLiteral("Proton %1.0").arg(match.captured(1));

    QString pretty = raw;
    pretty.replace(QLatin1Char('-'), QLatin1Char(' '));
    pretty.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QStringList words = pretty.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList capitalised;
    for (const QString &word : words) {
        QString w = word;
        if (!w.isEmpty() && w.at(0).isLower())
            w[0] = w.at(0).toUpper();
        capitalised << w;
    }
    pretty = capitalised.join(QLatin1Char(' '));
    if (!pretty.startsWith(QStringLiteral("Proton"), Qt::CaseInsensitive))
        pretty.prepend(QStringLiteral("Proton "));
    return pretty;
}

struct RuntimeGuess
{
    slm::RuntimeKind kind = slm::RuntimeKind::Unknown;
    QString detail;
};

RuntimeGuess guessRuntimeFromFiles(const QString &payload)
{
    RuntimeGuess guess;
    if (payload.isEmpty() || !QFileInfo(payload).isDir())
        return guess;

    bool hasElf = false;
    bool hasWindowsExe = false;
    bool hasShellLauncher = false;
    int inspected = 0;

    std::function<void(const QString &, int)> inspectDirectory = [&](const QString &directory, int depth) {
        QDir dir(directory);
        const QFileInfoList entries =
            dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &info : entries) {
            if (++inspected > 500)
                return;
            const QString suffix = info.suffix().toLower();
            if (suffix == QStringLiteral("exe")) {
                hasWindowsExe = true;
                continue;
            }
            if (suffix == QStringLiteral("sh") && info.isExecutable())
                hasShellLauncher = true;
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const QByteArray head = file.read(4);
            if (head == QByteArray("\x7f" "ELF", 4))
                hasElf = true;
        }
        if (depth <= 0)
            return;
        const QFileInfoList subdirs =
            dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo &subdir : subdirs) {
            if (++inspected > 500)
                return;
            inspectDirectory(subdir.absoluteFilePath(), depth - 1);
        }
    };

    inspectDirectory(payload, 1);
    if (hasElf || hasShellLauncher) {
        if (!hasWindowsExe) {
            guess.kind = slm::RuntimeKind::Native;
            guess.detail = QStringLiteral("Local Linux executable found");
        } else {
            guess.kind = slm::RuntimeKind::Unknown;
            guess.detail = QStringLiteral("Both Linux and Windows executables found");
        }
    } else if (hasWindowsExe) {
        guess.kind = slm::RuntimeKind::Proton;
        guess.detail = QStringLiteral("Windows executable, no local Linux executable");
    }
    return guess;
}

const QSet<quint32> &knownSystemAppIds()
{
    static const QSet<quint32> ids = {
        228980,  // Steamworks Common Redistributables
        1070560, // Steam Linux Runtime 1.0 (scout)
        1391110, // Steam Linux Runtime 2.0 (soldier)
        1628350, // Steam Linux Runtime 3.0 (sniper)
        4183110, // Steam Linux Runtime 4.0
        1493710, // Proton Experimental
        2805730, // Proton 9.0
        2348590, // Proton 8.0
        3658110, // Proton 10.0
        1161040, // Proton BattlEye Runtime
        1826330, // Proton EasyAntiCheat Runtime
    };
    return ids;
}

bool looksLikeSystemComponent(const AppManifest &manifest)
{
    if (knownSystemAppIds().contains(manifest.appId))
        return true;
    const QString name = manifest.name.toLower();
    const QString dir = manifest.installDir.toLower();
    if (name.startsWith(QStringLiteral("proton")) || dir.startsWith(QStringLiteral("proton")))
        return true;
    if (name.contains(QStringLiteral("steam linux runtime"))
        || dir.contains(QStringLiteral("steamlinuxruntime")))
        return true;
    if (name.contains(QStringLiteral("steamworks shared"))
        || dir.contains(QStringLiteral("steamworks shared")))
        return true;
    return false;
}

bool looksLikeTool(const AppManifest &manifest)
{
    const QString name = manifest.name.toLower();
    return name.endsWith(QStringLiteral("services")) || name.endsWith(QStringLiteral(" runtime"))
        || name.contains(QStringLiteral("redistributable")) || name.contains(QStringLiteral("sdk"))
        || name.contains(QStringLiteral("dedicated server"));
}

QString findArtwork(const QString &steamRoot, quint32 appId)
{
    const QString cachePath = joinPath(steamRoot, QStringLiteral("appcache/librarycache"));
    const QString appCache = joinPath(cachePath, QString::number(appId));
    // Current Steam clients store artwork in a per-app directory. Older clients
    // (and some community layouts) use flat <appid>_<type> files.
    const QStringList names = {
        QStringLiteral("library_600x900.jpg"),
        QStringLiteral("library_600x900.png"),
        QStringLiteral("header.jpg"),
        QStringLiteral("header.png"),
        QStringLiteral("logo.png"),
    };
    for (const QString &name : names) {
        const QString path = joinPath(appCache, name);
        if (QFileInfo::exists(path) && QFileInfo(path).isFile())
            return path;
    }
    for (const QString &name : names) {
        const QString path = joinPath(cachePath, QStringLiteral("%1_%2").arg(appId).arg(name));
        if (QFileInfo::exists(path) && QFileInfo(path).isFile())
            return path;
    }
    return {};
}

QVector<Issue> stateIssues(const AppManifest &manifest)
{
    QVector<Issue> issues;
    if (manifest.stateFlags != 4u) {
        issues.append({Severity::Blocker, QStringLiteral("state-flags"),
                       QStringLiteral("Game is not in a clean, fully installed state"),
                       QStringLiteral("StateFlags = %1 (%2). Finish updates/validation in Steam first.")
                           .arg(manifest.stateFlags)
                           .arg(manifest.stateFlagNames().join(QStringLiteral(", ")))});
    }
    return issues;
}

bool isDirectoryNoFollow(const QString &path, bool *isSymlink = nullptr)
{
    struct stat st {};
    const QByteArray encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &st) != 0) {
        if (isSymlink)
            *isSymlink = false;
        return false;
    }
    if (isSymlink)
        *isSymlink = S_ISLNK(st.st_mode);
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

void appendUniqueRoot(QVector<SteamRoot> &roots, const QString &path, QStringList *notes)
{
    if (path.isEmpty())
        return;
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return;
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return;
    const QString vdf = joinPath(canonical, QStringLiteral("steamapps/libraryfolders.vdf"));
    const QString steamapps = joinPath(canonical, QStringLiteral("steamapps"));
    if (!QFileInfo::exists(vdf) && !QFileInfo(steamapps).isDir())
        return;
    for (const SteamRoot &root : roots) {
        if (root.path == canonical)
            return;
    }
    roots.append({canonical, kindForPath(canonical)});
    if (notes)
        notes->append(QStringLiteral("Found Steam root: %1 (%2)").arg(canonical, kindForPath(canonical) == InstallKind::Flatpak ? QStringLiteral("Flatpak") : QStringLiteral("native")));
}

} // namespace

QVector<SteamRoot> detectSteamRoots(const QString &overridePath, QStringList *notes)
{
    QVector<SteamRoot> roots;
    const QString home = homeDir();

    if (!overridePath.isEmpty())
        appendUniqueRoot(roots, overridePath, notes);

    const QString envRoot = qEnvironmentVariable("SLM_STEAM_ROOT");
    if (!envRoot.isEmpty())
        appendUniqueRoot(roots, envRoot, notes);

    appendUniqueRoot(roots, joinPath(home, QStringLiteral(".steam/steam")), notes);
    appendUniqueRoot(roots, joinPath(home, QStringLiteral(".steam/root")), notes);
    appendUniqueRoot(roots, joinPath(home, QStringLiteral(".local/share/Steam")), notes);
    appendUniqueRoot(roots, joinPath(home, QStringLiteral(".var/app/com.valvesoftware.Steam/.local/share/Steam")), notes);
    appendUniqueRoot(roots, joinPath(home, QStringLiteral(".var/app/com.valvesoftware.Steam/.steam/steam")), notes);
    appendUniqueRoot(roots, joinPath(home, QStringLiteral("snap/steam/common/.local/share/Steam")), notes);

    return roots;
}

SteamScan scanSteam(const SteamRoot &root, const MountTable &mounts, QString *error)
{
    SteamScan scan;
    scan.root = root;
    scan.mounts = mounts.mounts();

    LibraryLoadResult loaded = loadLibraries(root, mounts);
    if (!loaded.ok) {
        if (error)
            *error = loaded.error;
        return scan;
    }
    scan.libraryFoldersPath = loaded.vdfPath;
    scan.libraryFoldersRaw = loaded.vdfRaw;
    scan.libraries = loaded.libraries;
    scan.issues = loaded.issues;
    const QMap<quint32, QString> compatTools = loadCompatToolMapping(root);

    for (const Library &library : scan.libraries) {
        if (library.compatdata.state == CompatDataState::BrokenSymlink) {
            scan.issues.append({Severity::Warning, QStringLiteral("compatdata-broken"),
                                QStringLiteral("Broken compatdata symlink in %1").arg(library.rawPath),
                                library.compatdata.error});
        } else if (library.compatdata.state == CompatDataState::Missing
                   && library.mount.isWindowsCompatible()) {
            scan.issues.append({Severity::Warning, QStringLiteral("compatdata-missing"),
                                QStringLiteral("No compatdata symlink in %1").arg(library.rawPath),
                                QStringLiteral("NTFS libraries should have a compatdata symlink pointing to a Linux filesystem. Use the NTFS Doctor before moving games there.")});
        } else if (library.compatdata.targetOnWindowsFs) {
            scan.issues.append({Severity::Warning, QStringLiteral("compatdata-on-windows-fs"),
                                QStringLiteral("compatdata for %1 resolves to a Windows filesystem").arg(library.rawPath),
                                QStringLiteral("Proton prefixes on NTFS/exFAT can be corrupted by invalid filenames.")});
        }
    }

    QMap<quint32, int> appIdCounts;
    for (const Library &library : scan.libraries) {
        const QString steamapps = library.steamappsPath();
        QDir dir(steamapps);
        const QStringList manifests = dir.entryList({QStringLiteral("appmanifest_*.acf")}, QDir::Files,
                                                    QDir::Name);
        for (const QString &manifestName : manifests) {
            const QString manifestPath = joinPath(steamapps, manifestName);
            ManifestLoadResult manifestResult = loadAppManifest(manifestPath);

            if (!manifestResult.ok) {
                static const QRegularExpression expression(
                    QStringLiteral(R"(^appmanifest_([0-9]{1,10})\.acf$)"));
                const QRegularExpressionMatch match = expression.match(manifestName);
                bool idOk = false;
                const quint32 appId = match.hasMatch()
                    ? match.captured(1).toUInt(&idOk, 10)
                    : 0;
                Game broken;
                broken.appId = idOk ? appId : 0;
                broken.name = QStringLiteral("Broken manifest: %1").arg(manifestName);
                broken.manifestPath = manifestPath;
                broken.libraryPath = library.path;
                broken.category = GameCategory::Broken;
                broken.issues.append({Severity::Blocker, QStringLiteral("manifest-invalid"),
                                      QStringLiteral("Cannot parse manifest"), manifestResult.error});
                scan.games.append(broken);
                continue;
            }

            const AppManifest &manifest = manifestResult.manifest;
            appIdCounts[manifest.appId]++;

            Game game;
            game.appId = manifest.appId;
            game.name = manifest.name.isEmpty() ? QStringLiteral("App %1").arg(manifest.appId)
                                                : manifest.name;
            game.installDir = manifest.installDir;
            game.sizeOnDisk = manifest.sizeOnDisk;
            game.stateFlags = manifest.stateFlags;
            game.manifestPath = manifestPath;
            game.libraryPath = library.path;
            game.artworkPath = findArtwork(root.path, manifest.appId);
            game.issues = stateIssues(manifest);
            game.lastPlayed = manifest.lastPlayed;
            game.buildId = manifest.buildId;

            const QString payloadPath = joinPath(library.steamappsPath(),
                                                 QStringLiteral("common/") + manifest.installDir);
            const QString compatTool = compatTools.value(manifest.appId);
            const RuntimeGuess filesystemGuess =
                isSafeInstallDirName(manifest.installDir) ? guessRuntimeFromFiles(payloadPath)
                                                          : RuntimeGuess{};
            if (compatTool.contains(QStringLiteral("proton"), Qt::CaseInsensitive)) {
                game.runtime = RuntimeKind::Proton;
                game.protonVersion = prettyProtonName(compatTool);
            } else if (!compatTool.isEmpty()) {
                game.runtime = RuntimeKind::Native;
                game.protonVersion = QStringLiteral("Compat tool: %1").arg(compatTool);
            } else {
                game.runtime = filesystemGuess.kind;
                if (game.runtime == RuntimeKind::Proton)
                    game.protonVersion = QStringLiteral("Proton (automatic)");
            }

            if (looksLikeSystemComponent(manifest))
                game.category = GameCategory::SystemComponent;
            else if (looksLikeTool(manifest))
                game.category = GameCategory::Tool;
            else
                game.category = GameCategory::Game;

            if (!isSafeInstallDirName(manifest.installDir)) {
                game.category = GameCategory::Broken;
                game.issues.append({Severity::Blocker, QStringLiteral("invalid-installdir"),
                                    QStringLiteral("Manifest has an unsafe install directory"),
                                    QStringLiteral("installdir = '%1'").arg(manifest.installDir)});
            } else {
                bool symlink = false;
                if (!isDirectoryNoFollow(payloadPath, &symlink)) {
                    game.issues.append({Severity::Blocker, QStringLiteral("payload-missing"),
                                        QStringLiteral("Game payload is missing or not a directory"),
                                        QStringLiteral("Expected: %1%2")
                                            .arg(payloadPath, symlink ? QStringLiteral(" (is a symlink)") : QString())});
                }
            }

            if (!library.writeAccess) {
                game.issues.append({Severity::Warning, QStringLiteral("library-readonly"),
                                    QStringLiteral("Steam library is not writable"),
                                    QStringLiteral("%1 may need remounting with correct uid/gid.")
                                        .arg(library.rawPath)});
            }

            scan.games.append(game);
        }
    }

    for (Game &game : scan.games) {
        if (game.appId == 0)
            continue;
        if (appIdCounts.value(game.appId) > 1) {
            game.issues.append({Severity::Blocker, QStringLiteral("duplicate-appid"),
                                QStringLiteral("This appid exists in more than one library"),
                                QStringLiteral("Resolve the duplicate installation before moving.")});
        }
    }

    if (error)
        error->clear();
    return scan;
}

} // namespace slm
