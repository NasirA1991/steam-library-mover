// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/doctor.h"
#include "core/health.h"
#include "core/executor.h"
#include "core/instance_lock.h"
#include "core/fileops.h"
#include "core/journal.h"
#include "core/mountinfo.h"
#include "core/planner.h"
#include "core/process_check.h"
#include "core/scanner.h"
#include "core/types.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

using namespace slm;

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

void printIssue(const Issue &issue, const QString &prefix = {})
{
    err << (issue.isBlocker() ? "BLOCKER" : "WARNING") << ": " << prefix << issue.message;
    if (!issue.detail.isEmpty())
        err << " (" << issue.detail << ")";
    err << '\n';
}

QJsonObject issueToJson(const Issue &issue)
{
    return QJsonObject{{QStringLiteral("severity"), issue.isBlocker() ? QStringLiteral("blocker")
                                                                     : QStringLiteral("warning")},
                       {QStringLiteral("code"), issue.code},
                       {QStringLiteral("message"), issue.message},
                       {QStringLiteral("detail"), issue.detail}};
}

QJsonArray issuesToJson(const QVector<Issue> &issues)
{
    QJsonArray array;
    for (const Issue &issue : issues)
        array.append(issueToJson(issue));
    return array;
}

SteamRoot chooseRoot(const QString &override, QStringList &notes)
{
    const QVector<SteamRoot> roots = detectSteamRoots(override, &notes);
    if (roots.isEmpty())
        return {};
    return roots.first();
}

bool requireClosedSteam(const SteamRoot &)
{
    const ProcessUsage usage = inspectProcesses({}, {});
    if (!usage.steamRunning)
        return true;
    err << "Steam is running. Close Steam completely, or pass --quit-steam to ask it to shut down.\n";
    return false;
}

QString describeGame(const Game &game)
{
    return QStringLiteral("%1  %2  %3  %4")
        .arg(game.appId, 8)
        .arg(game.name, -40)
        .arg(formatBytes(game.sizeOnDisk), 10)
        .arg(game.libraryPath);
}

int commandList(const QCommandLineParser &parser, const SteamRoot &root, const MountTable &mounts)
{
    QString error;
    SteamScan scan = scanSteam(root, mounts, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }
    const bool jsonMode = parser.isSet(QStringLiteral("json"));
    if (jsonMode) {
        QJsonObject object;
        object.insert(QStringLiteral("root"), scan.root.path);
        object.insert(QStringLiteral("kind"), scan.root.kindName());
        QJsonArray libraries;
        for (const Library &library : scan.libraries) {
            libraries.append(QJsonObject{
                {QStringLiteral("path"), library.path},
                {QStringLiteral("rawPath"), library.rawPath},
                {QStringLiteral("label"), library.label},
                {QStringLiteral("fs"), library.mount.fsType},
                {QStringLiteral("compatdata"), static_cast<int>(library.compatdata.state)},
                {QStringLiteral("games"), library.apps.size()},
            });
        }
        QJsonArray games;
        for (const Game &game : scan.games) {
            games.append(QJsonObject{
                {QStringLiteral("appid"), static_cast<qint64>(game.appId)},
                {QStringLiteral("name"), game.name},
                {QStringLiteral("installDir"), game.installDir},
                {QStringLiteral("sizeOnDisk"), static_cast<qint64>(game.sizeOnDisk)},
                {QStringLiteral("stateFlags"), static_cast<qint64>(game.stateFlags)},
                {QStringLiteral("category"), static_cast<int>(game.category)},
                {QStringLiteral("runtime"), static_cast<int>(game.runtime)},
                {QStringLiteral("protonVersion"), game.protonVersion},
                {QStringLiteral("lastPlayed"), static_cast<qint64>(game.lastPlayed)},
                {QStringLiteral("buildId"), static_cast<qint64>(game.buildId)},
                {QStringLiteral("library"), game.libraryPath},
                {QStringLiteral("issues"), issuesToJson(game.issues)},
            });
        }
        object.insert(QStringLiteral("libraries"), libraries);
        object.insert(QStringLiteral("games"), games);
        object.insert(QStringLiteral("issues"), issuesToJson(scan.issues));
        out << QJsonDocument(object).toJson(QJsonDocument::Indented);
        return 0;
    }

    out << "Steam root: " << scan.root.path << " (" << scan.root.kindName() << ")\n";
    for (const Library &library : scan.libraries) {
        out << "  " << library.rawPath << "  [" << library.mount.fsType << ", "
            << (library.writeAccess ? "rw" : "ro") << ", " << library.apps.size() << " apps]\n";
        if (library.compatdata.state == CompatDataState::Symlink)
            out << "      compatdata -> " << library.compatdata.target << '\n';
    }
    for (const Issue &issue : scan.issues)
        printIssue(issue);
    for (const Game &game : scan.games)
        out << describeGame(game) << '\n';
    return 0;
}

MoveOptions optionsFromParser(const QCommandLineParser &parser)
{
    MoveOptions options;
    options.moveShaderCache = !parser.isSet(QStringLiteral("no-shadercache"));
    options.moveWorkshop = !parser.isSet(QStringLiteral("no-workshop"));
    options.fullVerify = parser.value(QStringLiteral("verify")) != QStringLiteral("quick");
    options.allowNtfsNative = parser.isSet(QStringLiteral("allow-ntfs"));
    options.quitSteam = parser.isSet(QStringLiteral("quit-steam"));
    options.keepSourceBackup = parser.isSet(QStringLiteral("keep-source"));
    return options;
}

int commandPlan(const QCommandLineParser &parser, const SteamRoot &root, const MountTable &mounts,
                bool execute)
{
    bool appOk = false;
    const quint32 appId = parser.value(QStringLiteral("appid")).toUInt(&appOk, 10);
    if (!appOk || appId == 0) {
        err << "A valid appid is required.\n";
        return 1;
    }
    const QString destination = parser.value(QStringLiteral("to"));
    if (destination.isEmpty()) {
        err << "--to <SteamLibrary path> is required.\n";
        return 1;
    }

    MoveOptions options = optionsFromParser(parser);
    if (options.quitSteam) {
        QString shutdownError;
        if (!requestSteamShutdown(root.kind, &shutdownError)) {
            err << shutdownError << '\n';
            return 1;
        }
        QThread::msleep(1500);
    }

    QString error;
    SteamScan scan = scanSteam(root, mounts, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }
    const MovePlan plan = MovePlanner::plan(scan, appId, destination, options, mounts, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }

    const bool jsonMode = parser.isSet(QStringLiteral("json"));
    if (jsonMode) {
        QJsonObject object;
        object.insert(QStringLiteral("appid"), static_cast<qint64>(plan.game.appId));
        object.insert(QStringLiteral("name"), plan.game.name);
        object.insert(QStringLiteral("source"), plan.sourceLibraryPath);
        object.insert(QStringLiteral("destination"), plan.destLibraryPath);
        object.insert(QStringLiteral("totalBytes"), static_cast<qint64>(plan.totalBytes));
        object.insert(QStringLiteral("runtime"), static_cast<int>(plan.game.runtime));
        object.insert(QStringLiteral("protonVersion"), plan.game.protonVersion);
        object.insert(QStringLiteral("lastPlayed"), static_cast<qint64>(plan.game.lastPlayed));
        object.insert(QStringLiteral("buildId"), static_cast<qint64>(plan.game.buildId));
        QJsonArray components;
        for (const Component &component : plan.components) {
            components.append(QJsonObject{
                {QStringLiteral("kind"), componentKindName(component.kind)},
                {QStringLiteral("source"), component.source},
                {QStringLiteral("destination"), component.destination},
                {QStringLiteral("bytes"), static_cast<qint64>(component.bytes)},
                {QStringLiteral("skip"), component.skip},
            });
        }
        object.insert(QStringLiteral("components"), components);
        object.insert(QStringLiteral("issues"), issuesToJson(plan.issues));
        out << QJsonDocument(object).toJson(QJsonDocument::Indented);
    } else {
        out << "Move " << plan.game.name << " (" << plan.game.appId << ")\n";
        out << "  From: " << plan.sourceLibraryPath << '\n';
        out << "  To:   " << plan.destLibraryPath << '\n';
        for (const Component &component : plan.components) {
            out << "  - " << component.kindName() << ": " << formatBytes(component.bytes);
            if (component.skip)
                out << " [already shared/untouched]";
            out << '\n';
        }
        for (const Issue &issue : plan.issues)
            printIssue(issue);
    }

    if (plan.hasBlocker()) {
        err << "Move has blockers; nothing was changed.\n";
        return 2;
    }
    if (!execute || parser.isSet(QStringLiteral("dry-run")))
        return 0;
    if (!parser.isSet(QStringLiteral("yes"))) {
        err << "Refusing to move without --yes (use --dry-run to preview).\n";
        return 2;
    }
    if (!requireClosedSteam(root))
        return 2;

    QTextStream &human = jsonMode ? err : out;
    MoveExecutor executor;
    const MoveResult result = executor.execute(plan, mounts, [&human](const MoveProgress &progress) {
        human << '\r' << phaseName(progress.phase) << " " << progress.message;
        if (progress.bytesTotal > 0) {
            human << " " << (progress.bytesDone * 100 / progress.bytesTotal) << "%";
        }
        human.flush();
    });
    human << '\n';
    if (!result.ok) {
        err << result.error << '\n';
        if (!result.journalPath.isEmpty())
            err << "Journal: " << result.journalPath << '\n';
        return 3;
    }
    human << "Moved successfully. Start Steam to refresh the library.\n";
    for (const QString &backup : result.backupPaths)
        human << "Source backup: " << backup << '\n';
    for (const QString &warning : result.cleanupErrors)
        err << "Cleanup warning: " << warning << '\n';
    return 0;
}

int commandDoctor(const QCommandLineParser &parser, const SteamRoot &root, const MountTable &mounts)
{
    QString error;
    SteamScan scan = scanSteam(root, mounts, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }
    const QVector<Issue> issues = NtfsDoctor::inspect(scan);
    const bool jsonMode = parser.isSet(QStringLiteral("json"));
    if (jsonMode) {
        QJsonObject object;
        object.insert(QStringLiteral("root"), scan.root.path);
        object.insert(QStringLiteral("issues"), issuesToJson(issues));
        out << QJsonDocument(object).toJson(QJsonDocument::Indented);
        return 0;
    }
    out << "Steam root: " << scan.root.path << '\n';
    for (const Library &library : scan.libraries) {
        out << library.rawPath << "  fs=" << library.mount.fsType << "  compatdata=";
        switch (library.compatdata.state) {
        case CompatDataState::Missing:
            out << "missing";
            break;
        case CompatDataState::Directory:
            out << "directory";
            break;
        case CompatDataState::Symlink:
            out << "symlink -> " << library.compatdata.target;
            break;
        case CompatDataState::BrokenSymlink:
            out << "broken";
            break;
        }
        out << '\n';
    }
    for (const Issue &issue : issues)
        printIssue(issue);
    out << "Suggested compatdata target: " << NtfsDoctor::defaultCompatDataTarget(scan) << '\n';
    return 0;
}

int commandFixCompatData(const QCommandLineParser &parser, const SteamRoot &root,
                         const MountTable &mounts)
{
    const QString libraryPath = parser.value(QStringLiteral("library"));
    if (libraryPath.isEmpty()) {
        err << "--library <path> is required.\n";
        return 1;
    }
    if (!requireClosedSteam(root))
        return 2;
    QString error;
    SteamScan scan = scanSteam(root, mounts, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }
    const Library *library = scan.findLibrary(libraryPath);
    if (!library) {
        err << "Library not found: " << libraryPath << '\n';
        return 1;
    }
    QString target = parser.value(QStringLiteral("to"));
    if (target.isEmpty())
        target = NtfsDoctor::defaultCompatDataTarget(scan);
    const bool apply = parser.isSet(QStringLiteral("apply"));
    if (!NtfsDoctor::fixCompatData(*library, target, mounts, apply, &error)) {
        err << error << '\n';
        return 1;
    }
    out << (apply ? "Compatdata symlink configured: " : "Dry run OK: ")
        << library->compatdataPath() << " -> " << target << '\n';
    return 0;
}

int commandHealth(const QCommandLineParser &parser, const SteamRoot &root,
                    const MountTable &mounts)
{
    QString error;
    const SteamScan scan = scanSteam(root, mounts, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }
    const QVector<HealthFinding> findings = LibraryHealth::scan(scan, &error);
    if (!error.isEmpty()) {
        err << error << '\n';
        return 1;
    }

    if (parser.isSet(QStringLiteral("apply"))) {
        if (!parser.isSet(QStringLiteral("yes"))) {
            err << "Add --yes to quarantine lock/temp files.\n";
            return 1;
        }
        QVector<HealthFinding> actionable;
        for (const HealthFinding &finding : findings) {
            if (finding.quarantineable)
                actionable.append(finding);
        }
        if (actionable.isEmpty()) {
            out << "No quarantineable lock/temp files.\n";
        } else if (!LibraryHealth::quarantine(scan, actionable, &error)) {
            err << error << '\n';
            return 1;
        } else {
            out << "Quarantined " << actionable.size()
                << " lock/temp file(s) into hidden .slm-quarantine folders.\n";
        }
    }

    if (parser.isSet(QStringLiteral("json"))) {
        QJsonArray array;
        for (const HealthFinding &finding : findings) {
            array.append(QJsonObject{
                {QStringLiteral("severity"),
                 static_cast<int>(finding.severity)},
                {QStringLiteral("kind"), static_cast<int>(finding.kind)},
                {QStringLiteral("library"), finding.libraryPath},
                {QStringLiteral("path"), finding.path},
                {QStringLiteral("title"), finding.title},
                {QStringLiteral("detail"), finding.detail},
                {QStringLiteral("quarantineable"), finding.quarantineable},
            });
        }
        QJsonObject object;
        object.insert(QStringLiteral("findings"), array);
        out << QJsonDocument(object).toJson(QJsonDocument::Indented);
        return 0;
    }

    int critical = 0;
    int warnings = 0;
    int infos = 0;
    for (const HealthFinding &finding : findings) {
        if (finding.severity == HealthSeverity::Critical)
            ++critical;
        else if (finding.severity == HealthSeverity::Warning)
            ++warnings;
        else
            ++infos;
        out << LibraryHealth::severityName(finding.severity) << "  "
            << LibraryHealth::kindName(finding.kind) << "  " << finding.path;
        if (!finding.title.isEmpty())
            out << "  —  " << finding.title;
        if (!finding.detail.isEmpty())
            out << "  (" << finding.detail << ')';
        out << '\n';
    }
    out << "Health: " << critical << " critical, " << warnings << " warnings, " << infos
        << " informational.\n";
    return 0;
}

int commandRecover(const QCommandLineParser &parser, const MountTable &mounts)
{
    if (parser.isSet(QStringLiteral("list"))) {
        const QList<QJsonObject> operations = MoveExecutor::unfinishedOperations();
        if (operations.isEmpty()) {
            out << "No unfinished operations.\n";
            return 0;
        }
        for (const QJsonObject &operation : operations) {
            out << operation.value(QStringLiteral("opId")).toString() << "  "
                << operation.value(QStringLiteral("state")).toString() << "  app "
                << operation.value(QStringLiteral("appId")).toString() << "  "
                << operation.value(QStringLiteral("name")).toString() << "  phase "
                << operation.value(QStringLiteral("phase")).toString() << '\n';
        }
        return 0;
    }
    const bool resume = parser.isSet(QStringLiteral("resume"));
    if (resume && parser.isSet(QStringLiteral("rollback"))) {
        err << "Use either --resume or --rollback, not both.\n";
        return 1;
    }
    const QString opId = parser.value(resume ? QStringLiteral("resume")
                                              : QStringLiteral("rollback"));
    if (opId.isEmpty()) {
        err << "Use --list, --resume <opId>, or --rollback <opId>.\n";
        return 1;
    }
    if (!JournalStore::isValidOperationId(opId)) {
        err << "Invalid operation id: " << opId << '\n';
        return 1;
    }
    const QJsonObject journal = JournalStore::readObject(JournalStore::journalPath(opId));
    if (journal.isEmpty()) {
        err << "Operation not found: " << opId << '\n';
        return 1;
    }
    QString error;
    if (resume) {
        MoveExecutor executor;
        const MoveResult result = executor.resumeOperation(journal, mounts);
        if (!result.ok) {
            err << result.error << '\n';
            return 1;
        }
        out << "Resumed and completed " << opId << '\n';
        return 0;
    }
    if (!MoveExecutor::rollbackOperation(journal, &error)) {
        err << error << '\n';
        return 1;
    }
    out << "Rolled back " << opId << '\n';
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("steam-library-mover"));
    app.setApplicationVersion(QString::fromLatin1(SLM_VERSION));
    app.setOrganizationName(QStringLiteral("SteamLibraryMover"));

    slm::InstanceLock instanceLock;
    QString lockError;
    if (!instanceLock.acquire(&lockError)) {
        err << lockError << "\n";
        return 1;
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Move installed Steam games between Steam libraries safely. Steam must be closed."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("steam-root"), QStringLiteral("Steam installation root"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("json"), QStringLiteral("Emit JSON")});
    parser.addOption({QStringLiteral("appid"), QStringLiteral("Steam appid"), QStringLiteral("id")});
    parser.addOption({QStringLiteral("to"), QStringLiteral("Destination SteamLibrary path"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("dry-run"), QStringLiteral("Do not modify anything")});
    parser.addOption({QStringLiteral("yes"), QStringLiteral("Confirm a move or health quarantine")});
    parser.addOption({QStringLiteral("verify"),
                      QStringLiteral("full (default) or quick (size/path only; not recommended)"),
                      QStringLiteral("mode"), QStringLiteral("full")});
    parser.addOption({QStringLiteral("no-shadercache"), QStringLiteral("Do not move shader cache")});
    parser.addOption({QStringLiteral("no-workshop"), QStringLiteral("Do not move workshop content")});
    parser.addOption({QStringLiteral("allow-ntfs"), QStringLiteral("Acknowledge Windows-filesystem risks")});
    parser.addOption({QStringLiteral("quit-steam"), QStringLiteral("Ask Steam to shut down")});
    parser.addOption({QStringLiteral("keep-source"), QStringLiteral("Leave moved source payload as hidden backups")});
    parser.addOption({QStringLiteral("library"), QStringLiteral("Steam library path"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("apply"), QStringLiteral("Apply doctor changes")});
    parser.addOption({QStringLiteral("list"), QStringLiteral("List unfinished operations")});
    parser.addOption({QStringLiteral("rollback"), QStringLiteral("Rollback operation id"), QStringLiteral("id")});
    parser.addOption({QStringLiteral("resume"), QStringLiteral("Resume an interrupted operation id"),
                      QStringLiteral("id")});
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("list | plan | move | doctor | fix-compatdata | recover | health"),
                                 QStringLiteral("[command]"));
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    const QString command = positional.value(0, QStringLiteral("list"));
    const QString verifyMode = parser.value(QStringLiteral("verify"));
    if (verifyMode != QStringLiteral("full") && verifyMode != QStringLiteral("quick")) {
        err << "Invalid --verify value: " << verifyMode << " (use full or quick)\n";
        return 1;
    }

    QStringList notes;
    const SteamRoot root = chooseRoot(parser.value(QStringLiteral("steam-root")), notes);
    if (root.path.isEmpty()) {
        err << "No Steam installation found. Use --steam-root <path>.\n";
        return 1;
    }
    QString mountError;
    const MountTable mounts = MountTable::load(&mountError);

    if (command == QStringLiteral("list"))
        return commandList(parser, root, mounts);
    if (command == QStringLiteral("plan"))
        return commandPlan(parser, root, mounts, false);
    if (command == QStringLiteral("move"))
        return commandPlan(parser, root, mounts, true);
    if (command == QStringLiteral("doctor"))
        return commandDoctor(parser, root, mounts);
    if (command == QStringLiteral("fix-compatdata"))
        return commandFixCompatData(parser, root, mounts);
    if (command == QStringLiteral("recover"))
        return commandRecover(parser, mounts);
    if (command == QStringLiteral("health"))
        return commandHealth(parser, root, mounts);

    err << "Unknown command: " << command << '\n';
    return 1;
}
