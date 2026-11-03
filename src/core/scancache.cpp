// SPDX-License-Identifier: GPL-3.0-or-later
#include "scancache.h"

#include "fileops.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <utility>

namespace slm::ScanCache {

namespace {

constexpr int kVersion = 1;

QJsonObject issueToJson(const Issue &issue)
{
    QJsonObject object;
    object.insert(QStringLiteral("severity"), static_cast<int>(issue.severity));
    object.insert(QStringLiteral("code"), issue.code);
    object.insert(QStringLiteral("message"), issue.message);
    object.insert(QStringLiteral("detail"), issue.detail);
    return object;
}

Issue issueFromJson(const QJsonObject &object)
{
    Issue issue;
    issue.severity = static_cast<Severity>(object.value(QStringLiteral("severity")).toInt());
    issue.code = object.value(QStringLiteral("code")).toString();
    issue.message = object.value(QStringLiteral("message")).toString();
    issue.detail = object.value(QStringLiteral("detail")).toString();
    return issue;
}

QJsonObject mountToJson(const MountInfo &mount)
{
    QJsonObject object;
    object.insert(QStringLiteral("mountPoint"), mount.mountPoint);
    object.insert(QStringLiteral("fsType"), mount.fsType);
    object.insert(QStringLiteral("source"), mount.source);
    object.insert(QStringLiteral("options"), QJsonArray::fromStringList(mount.options));
    object.insert(QStringLiteral("mountId"), static_cast<double>(mount.mountId));
    object.insert(QStringLiteral("totalBytes"), static_cast<double>(mount.totalBytes));
    object.insert(QStringLiteral("freeBytes"), static_cast<double>(mount.freeBytes));
    object.insert(QStringLiteral("readOnly"), mount.readOnly);
    object.insert(QStringLiteral("fsClass"), static_cast<int>(mount.fsClass));
    // device is dev_t and is not needed for display.
    return object;
}

MountInfo mountFromJson(const QJsonObject &object)
{
    MountInfo mount;
    mount.mountPoint = object.value(QStringLiteral("mountPoint")).toString();
    mount.fsType = object.value(QStringLiteral("fsType")).toString();
    mount.source = object.value(QStringLiteral("source")).toString();
    for (const QJsonValue &option : object.value(QStringLiteral("options")).toArray())
        mount.options.append(option.toString());
    mount.mountId = static_cast<quint64>(object.value(QStringLiteral("mountId")).toDouble());
    mount.totalBytes = static_cast<quint64>(object.value(QStringLiteral("totalBytes")).toDouble());
    mount.freeBytes = static_cast<quint64>(object.value(QStringLiteral("freeBytes")).toDouble());
    mount.readOnly = object.value(QStringLiteral("readOnly")).toBool();
    mount.fsClass = static_cast<FsClass>(object.value(QStringLiteral("fsClass")).toInt());
    return mount;
}

QJsonObject compatToJson(const CompatDataInfo &compat)
{
    QJsonObject object;
    object.insert(QStringLiteral("state"), static_cast<int>(compat.state));
    object.insert(QStringLiteral("path"), compat.path);
    object.insert(QStringLiteral("linkTarget"), compat.linkTarget);
    object.insert(QStringLiteral("target"), compat.target);
    object.insert(QStringLiteral("targetOnWindowsFs"), compat.targetOnWindowsFs);
    object.insert(QStringLiteral("error"), compat.error);
    return object;
}

CompatDataInfo compatFromJson(const QJsonObject &object)
{
    CompatDataInfo compat;
    compat.state = static_cast<CompatDataState>(object.value(QStringLiteral("state")).toInt());
    compat.path = object.value(QStringLiteral("path")).toString();
    compat.linkTarget = object.value(QStringLiteral("linkTarget")).toString();
    compat.target = object.value(QStringLiteral("target")).toString();
    compat.targetOnWindowsFs = object.value(QStringLiteral("targetOnWindowsFs")).toBool();
    compat.error = object.value(QStringLiteral("error")).toString();
    return compat;
}

QJsonObject gameToJson(const Game &game)
{
    QJsonObject object;
    object.insert(QStringLiteral("appId"), static_cast<double>(game.appId));
    object.insert(QStringLiteral("name"), game.name);
    object.insert(QStringLiteral("installDir"), game.installDir);
    object.insert(QStringLiteral("sizeOnDisk"), QString::number(game.sizeOnDisk));
    object.insert(QStringLiteral("stateFlags"), static_cast<double>(game.stateFlags));
    object.insert(QStringLiteral("lastPlayed"), static_cast<double>(game.lastPlayed));
    object.insert(QStringLiteral("buildId"), QString::number(game.buildId));
    object.insert(QStringLiteral("manifestPath"), game.manifestPath);
    object.insert(QStringLiteral("libraryPath"), game.libraryPath);
    object.insert(QStringLiteral("category"), static_cast<int>(game.category));
    object.insert(QStringLiteral("runtime"), static_cast<int>(game.runtime));
    object.insert(QStringLiteral("protonVersion"), game.protonVersion);
    object.insert(QStringLiteral("artworkPath"), game.artworkPath);
    QJsonArray issues;
    for (const Issue &issue : game.issues)
        issues.append(issueToJson(issue));
    object.insert(QStringLiteral("issues"), issues);
    return object;
}

Game gameFromJson(const QJsonObject &object)
{
    Game game;
    game.appId = static_cast<quint32>(object.value(QStringLiteral("appId")).toDouble());
    game.name = object.value(QStringLiteral("name")).toString();
    game.installDir = object.value(QStringLiteral("installDir")).toString();
    game.sizeOnDisk = object.value(QStringLiteral("sizeOnDisk")).toString().toULongLong();
    game.stateFlags = static_cast<quint32>(object.value(QStringLiteral("stateFlags")).toDouble());
    game.lastPlayed = static_cast<qint64>(object.value(QStringLiteral("lastPlayed")).toDouble());
    game.buildId = object.value(QStringLiteral("buildId")).toString().toULongLong();
    game.manifestPath = object.value(QStringLiteral("manifestPath")).toString();
    game.libraryPath = object.value(QStringLiteral("libraryPath")).toString();
    game.category = static_cast<GameCategory>(object.value(QStringLiteral("category")).toInt());
    game.runtime = static_cast<RuntimeKind>(object.value(QStringLiteral("runtime")).toInt());
    game.protonVersion = object.value(QStringLiteral("protonVersion")).toString();
    game.artworkPath = object.value(QStringLiteral("artworkPath")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("issues")).toArray())
        game.issues.append(issueFromJson(value.toObject()));
    return game;
}

QJsonObject libraryToJson(const Library &library)
{
    QJsonObject object;
    object.insert(QStringLiteral("rawPath"), library.rawPath);
    object.insert(QStringLiteral("path"), library.path);
    object.insert(QStringLiteral("label"), library.label);
    object.insert(QStringLiteral("vdfKey"), library.vdfKey);
    object.insert(QStringLiteral("rootPath"), library.rootPath);
    object.insert(QStringLiteral("mount"), mountToJson(library.mount));
    object.insert(QStringLiteral("compatdata"), compatToJson(library.compatdata));
    QJsonObject apps;
    for (auto it = library.apps.constBegin(); it != library.apps.constEnd(); ++it)
        apps.insert(QString::number(it.key()), QString::number(it.value()));
    object.insert(QStringLiteral("apps"), apps);
    object.insert(QStringLiteral("registered"), library.registered);
    object.insert(QStringLiteral("writeAccess"), library.writeAccess);
    return object;
}

Library libraryFromJson(const QJsonObject &object)
{
    Library library;
    library.rawPath = object.value(QStringLiteral("rawPath")).toString();
    library.path = object.value(QStringLiteral("path")).toString();
    library.label = object.value(QStringLiteral("label")).toString();
    library.vdfKey = object.value(QStringLiteral("vdfKey")).toString();
    library.rootPath = object.value(QStringLiteral("rootPath")).toString();
    library.mount = mountFromJson(object.value(QStringLiteral("mount")).toObject());
    library.compatdata = compatFromJson(object.value(QStringLiteral("compatdata")).toObject());
    const QJsonObject apps = object.value(QStringLiteral("apps")).toObject();
    for (auto it = apps.constBegin(); it != apps.constEnd(); ++it)
        library.apps.insert(it.key().toUInt(), it.value().toString().toULongLong());
    library.registered = object.value(QStringLiteral("registered")).toBool();
    library.writeAccess = object.value(QStringLiteral("writeAccess")).toBool();
    return library;
}

} // namespace

QString cachePath(const SteamRoot &root)
{
    QString base = qEnvironmentVariable("SLM_CACHE_DIR");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath() + QStringLiteral("/steam-library-mover");
    const QByteArray key = (root.path + QLatin1Char('|') + QString::number(static_cast<int>(root.kind)))
                               .toUtf8();
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
    return base + QStringLiteral("/scan-") + hash + QStringLiteral(".json");
}

bool save(const SteamScan &scan, QString *error)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("rootPath"), scan.root.path);
    root.insert(QStringLiteral("rootKind"), static_cast<int>(scan.root.kind));
    root.insert(QStringLiteral("libraryFoldersPath"), scan.libraryFoldersPath);
    root.insert(QStringLiteral("libraryFoldersRaw"),
                QString::fromLatin1(scan.libraryFoldersRaw.toBase64()));
    QJsonArray libraries;
    for (const Library &library : scan.libraries)
        libraries.append(libraryToJson(library));
    root.insert(QStringLiteral("libraries"), libraries);
    QJsonArray games;
    for (const Game &game : scan.games)
        games.append(gameToJson(game));
    root.insert(QStringLiteral("games"), games);
    QJsonArray issues;
    for (const Issue &issue : scan.issues)
        issues.append(issueToJson(issue));
    root.insert(QStringLiteral("issues"), issues);
    QJsonArray mounts;
    for (const MountInfo &mount : scan.mounts)
        mounts.append(mountToJson(mount));
    root.insert(QStringLiteral("mounts"), mounts);

    const QString path = cachePath(scan.root);
    if (!fsutil::ensureDir(QFileInfo(path).absolutePath(), error))
        return false;
    if (!fsutil::writeFileAtomic(path, QJsonDocument(root).toJson(QJsonDocument::Compact), 0600,
                                 error))
        return false;
    return true;
}

bool load(const SteamRoot &root, SteamScan *scan, QString *error)
{
    if (!scan) {
        if (error)
            *error = QStringLiteral("No scan target");
        return false;
    }
    QFile file(cachePath(root));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("No cached scan: %1").arg(file.errorString());
        return false;
    }
    if (file.size() > 16 * 1024 * 1024) {
        if (error)
            *error = QStringLiteral("Cached scan is too large");
        return false;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid cached scan");
        return false;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt() != kVersion) {
        if (error)
            *error = QStringLiteral("Cached scan version is not supported");
        return false;
    }
    if (object.value(QStringLiteral("rootPath")).toString() != root.path
        || object.value(QStringLiteral("rootKind")).toInt() != static_cast<int>(root.kind)) {
        if (error)
            *error = QStringLiteral("Cached scan belongs to a different Steam root");
        return false;
    }

    SteamScan loaded;
    loaded.root = root;
    loaded.libraryFoldersPath = object.value(QStringLiteral("libraryFoldersPath")).toString();
    loaded.libraryFoldersRaw =
        QByteArray::fromBase64(object.value(QStringLiteral("libraryFoldersRaw")).toString().toLatin1());
    for (const QJsonValue &value : object.value(QStringLiteral("libraries")).toArray())
        loaded.libraries.append(libraryFromJson(value.toObject()));
    for (const QJsonValue &value : object.value(QStringLiteral("games")).toArray())
        loaded.games.append(gameFromJson(value.toObject()));
    for (const QJsonValue &value : object.value(QStringLiteral("issues")).toArray())
        loaded.issues.append(issueFromJson(value.toObject()));
    for (const QJsonValue &value : object.value(QStringLiteral("mounts")).toArray())
        loaded.mounts.append(mountFromJson(value.toObject()));
    if (loaded.libraries.isEmpty()) {
        if (error)
            *error = QStringLiteral("Cached scan has no libraries");
        return false;
    }
    *scan = std::move(loaded);
    if (error)
        error->clear();
    return true;
}

} // namespace slm::ScanCache
