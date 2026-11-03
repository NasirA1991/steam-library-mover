// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <sys/types.h>

namespace slm {

enum class InstallKind { Native, Flatpak, Snap };

enum class CompatDataState { Missing, Directory, Symlink, BrokenSymlink };

enum class FsClass {
    Posix,
    Ntfs,
    Exfat,
    Fat32,
    Network,
    Other,
};

enum class GameCategory {
    Game,
    Tool,
    SystemComponent,
    Unmanaged,
    Broken,
};

enum class RuntimeKind { Unknown, Proton, Native };

enum class Severity { Warning, Blocker };

enum class ComponentKind {
    Game,
    Manifest,
    Prefix,
    ShaderCache,
    WorkshopContent,
    WorkshopManifest,
};

enum class Phase {
    Planned,
    CopyingGame,
    CopyingAux,
    Verifying,
    Syncing,
    WritingManifest,
    UpdatingVdf,
    RemovingSourceManifest,
    RemovingSourcePayload,
    Done,
    Failed,
    Cancelled,
};

struct Issue
{
    Severity severity = Severity::Warning;
    QString code;
    QString message;
    QString detail;

    bool isBlocker() const { return severity == Severity::Blocker; }
};

struct MountInfo
{
    QString mountPoint;
    QString fsType;
    QString source;
    QStringList options;
    quint64 mountId = 0;
    quint64 totalBytes = 0;
    quint64 freeBytes = 0;
    dev_t device = 0;
    bool readOnly = false;
    FsClass fsClass = FsClass::Other;

    bool hasOption(const QString &name) const;
    bool isWindowsCompatible() const;
    bool isNetwork() const;
    bool isPosix() const;
    QString describe() const;
};

struct SteamRoot
{
    QString path;
    InstallKind kind = InstallKind::Native;

    QString kindName() const;
};

struct CompatDataInfo
{
    CompatDataState state = CompatDataState::Missing;
    QString path;          // <library>/steamapps/compatdata
    QString linkTarget;    // raw readlink value if a symlink
    QString target;        // resolved target directory, empty if broken
    bool targetOnWindowsFs = false;
    QString error;
};

struct Library
{
    QString rawPath;   // path as written in libraryfolders.vdf
    QString path;      // canonical/absolute path used for I/O
    QString label;
    QString vdfKey;    // "0", "1", ...
    QString rootPath;  // owning Steam root
    MountInfo mount;
    CompatDataInfo compatdata;
    QMap<quint32, quint64> apps;
    bool registered = true;
    bool writeAccess = false;

    QString steamappsPath() const;
    QString compatdataPath() const;
};

struct AppManifest
{
    quint32 appId = 0;
    QString name;
    QString installDir;
    quint32 stateFlags = 0;
    quint64 sizeOnDisk = 0;
    qint64 lastPlayed = 0;
    quint64 buildId = 0;
    QString path;

    bool isFullyInstalled() const;
    bool hasNonIdleFlags() const;
    QStringList stateFlagNames() const;
};

struct Game
{
    quint32 appId = 0;
    QString name;
    QString installDir;
    quint64 sizeOnDisk = 0;
    quint32 stateFlags = 0;
    qint64 lastPlayed = 0;
    quint64 buildId = 0;
    QString manifestPath;
    QString libraryPath;
    GameCategory category = GameCategory::Game;
    RuntimeKind runtime = RuntimeKind::Unknown;
    QString protonVersion;
    QVector<Issue> issues;
    QString artworkPath;

    bool hasBlocker() const;
    bool isMovable() const;
};

struct SteamScan
{
    SteamRoot root;
    QString libraryFoldersPath;
    QByteArray libraryFoldersRaw;
    QVector<Library> libraries;
    QVector<Game> games;
    QVector<Issue> issues;
    QVector<MountInfo> mounts;

    const Library *findLibrary(const QString &pathOrRawPath) const;
    const Game *findGame(quint32 appId) const;
};

struct MoveOptions
{
    bool moveShaderCache = true;
    bool moveWorkshop = true;
    bool fullVerify = false;
    bool allowNtfsNative = false; // explicit acknowledgement that native titles may not run
    bool quitSteam = false;       // explicit permission to ask Steam to quit
    bool keepSourceBackup = false; // leave moved source payload as a hidden backup
};

struct Component
{
    ComponentKind kind = ComponentKind::Game;
    QString source;
    QString destination;
    quint64 bytes = 0;
    bool required = true;
    bool skip = false;
    QString note;

    QString kindName() const;
};

struct PreflightSnapshot
{
    QByteArray manifestHash;
    QByteArray vdfHash;
    qint64 manifestMtime = 0;
    qint64 manifestSize = -1;
    qint64 vdfMtime = 0;
    qint64 vdfSize = -1;
    QString sourceMountId;
    QString destinationMountId;
    QString prefixMountId;
    quint64 sourceDevice = 0;
    quint64 destinationDevice = 0;
    quint64 prefixDevice = 0;
    QString prefixCheckPath;
};

struct MovePlan
{
    Game game;
    QString sourceLibraryPath;
    QString destLibraryPath;
    QString libraryFoldersPath;
    QVector<Component> components;
    QMap<QString, quint64> bytesPerMount; // mount point -> component bytes
    QMap<QString, quint64> requiredPerMount; // mount point -> bytes including safety reserve
    QMap<QString, quint64> freePerMount;
    quint64 totalBytes = 0;
    QVector<Issue> issues;
    MoveOptions options;
    PreflightSnapshot snapshot;

    bool hasBlocker() const;
    QVector<Issue> blockers() const;
    QVector<Issue> warnings() const;
    const Component *findComponent(ComponentKind kind) const;
};

struct MoveProgress
{
    Phase phase = Phase::Planned;
    QString message;
    QString currentFile;
    quint64 bytesDone = 0;
    quint64 bytesTotal = 0;
};

struct MoveResult
{
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString journalPath;
    QString logPath;
    QStringList cleanupErrors;
    QStringList backupPaths;
};

struct RunResult
{
    bool ok = false;
    QString error;
};

QString phaseName(Phase phase);
QString componentKindName(ComponentKind kind);
QString gameCategoryName(GameCategory category);
QString runtimeKindName(RuntimeKind kind);
QString formatBytes(quint64 bytes);
QString decodeMountInfoEscapes(const QByteArray &input);
QString canonicalOrAbsolute(const QString &path);
bool pathExistsNoFollow(const QString &path);
bool pathIsWithin(const QString &candidate, const QString &parent);
QByteArray sha256File(const QString &path, QString *error = nullptr);

} // namespace slm

Q_DECLARE_METATYPE(slm::SteamScan)
Q_DECLARE_METATYPE(slm::MovePlan)
Q_DECLARE_METATYPE(slm::MoveProgress)
Q_DECLARE_METATYPE(slm::MoveResult)
