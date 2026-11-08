// SPDX-License-Identifier: GPL-3.0-or-later
#include "process_check.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

#include <algorithm>

#include <unistd.h>

namespace slm {

namespace {

ProcessInspector g_testInspector;

QString baseName(const QString &value)
{
    return QFileInfo(value).fileName();
}

QString stripDeleted(const QString &value)
{
    QString result = value;
    if (result.endsWith(QStringLiteral(" (deleted)")))
        result.chop(10);
    return result;
}

bool processMatchesGame(const QStringList &arguments, const QString &executable, const QString &cwd,
                        const QString &gameDir)
{
    if (!gameDir.isEmpty()) {
        if (!executable.isEmpty() && pathIsWithin(stripDeleted(executable), gameDir))
            return true;
        if (!cwd.isEmpty() && pathIsWithin(cwd, gameDir))
            return true;
        for (const QString &argument : arguments) {
            if (argument.startsWith(QLatin1Char('/')) && pathIsWithin(argument, gameDir))
                return true;
        }
    }
    return false;
}

QStringList readNullSeparated(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    QStringList result;
    const QList<QByteArray> parts = data.split('\0');
    for (const QByteArray &part : parts) {
        if (!part.isEmpty())
            result.append(QString::fromUtf8(part));
    }
    return result;
}

} // namespace

bool isSteamProcessName(const QString &name)
{
    static const QSet<QString> names = {
        QStringLiteral("steam"),
        QStringLiteral("steamwebhelper"),
        QStringLiteral("steamerrorreporter"),
        QStringLiteral("steam-runtime"),
        QStringLiteral("steam-runtime-launcher-service"),
    };
    return names.contains(name.toLower());
}

void setProcessInspectorForTesting(ProcessInspector inspector)
{
    g_testInspector = std::move(inspector);
}

ProcessUsage inspectProcesses(const QStringList &prefixes, const QStringList &gameDirs)
{
    if (g_testInspector)
        return g_testInspector(prefixes, gameDirs);

    ProcessUsage usage;
    const qint64 self = QCoreApplication::applicationPid();
    QDir proc(QStringLiteral("/proc"));
    const QStringList entries = proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    QSet<qint64> seenPids;

    const QStringList canonicalPrefixes = [&]() {
        QStringList result;
        for (const QString &prefix : prefixes) {
            if (!prefix.isEmpty())
                result.append(canonicalOrAbsolute(prefix));
        }
        return result;
    }();
    const QStringList canonicalGameDirs = [&]() {
        QStringList result;
        for (const QString &dir : gameDirs) {
            if (!dir.isEmpty())
                result.append(canonicalOrAbsolute(dir));
        }
        return result;
    }();

    for (const QString &entry : entries) {
        bool pidOk = false;
        const qint64 pid = entry.toLongLong(&pidOk);
        if (!pidOk || pid <= 0 || pid == self || seenPids.contains(pid))
            continue;
        seenPids.insert(pid);

        const QString procPath = QStringLiteral("/proc/%1").arg(pid);
        const QStringList arguments = readNullSeparated(procPath + QStringLiteral("/cmdline"));
        if (arguments.isEmpty())
            continue; // kernel thread or unreadable

        QString executable;
        const QFileInfo exeInfo(procPath + QStringLiteral("/exe"));
        executable = exeInfo.isSymLink() ? exeInfo.symLinkTarget() : QString();
        if (executable.isEmpty()) {
            const QByteArray target = QFile::symLinkTarget(procPath + QStringLiteral("/exe")).toUtf8();
            executable = QString::fromUtf8(target);
        }

        QString cwd;
        const QFileInfo cwdInfo(procPath + QStringLiteral("/cwd"));
        cwd = cwdInfo.isSymLink() ? cwdInfo.symLinkTarget() : QString();

        const QString first = arguments.value(0);
        const QString firstBase = baseName(first);
        // Match only the executable or the kernel-recorded process name. Scanning
        // every argument makes `less steam` or `grep steam` look like Steam.
        QString comm;
        QFile commFile(procPath + QStringLiteral("/comm"));
        if (commFile.open(QIODevice::ReadOnly))
            comm = QString::fromUtf8(commFile.read(256)).trimmed();
        const QString executableBase = baseName(executable);
        const bool steamProcess = isSteamProcessName(executableBase)
            || isSteamProcessName(comm)
            || isSteamProcessName(firstBase);

        if (steamProcess) {
            usage.steamRunning = true;
            usage.steamPids.append(pid);
            usage.steamNames.append(firstBase);
        }

        const QString environPath = procPath + QStringLiteral("/environ");
        QFile environ(environPath);
        if (environ.open(QIODevice::ReadOnly)) {
            const QByteArray envData = environ.readAll();
            const QList<QByteArray> vars = envData.split('\0');
            for (const QByteArray &var : vars) {
                if (!var.startsWith("STEAM_COMPAT_DATA_PATH="))
                    continue;
                const QString value = QString::fromUtf8(var.mid(23));
                const QString canonical = canonicalOrAbsolute(value);
                for (const QString &prefix : canonicalPrefixes) {
                    if (!prefix.isEmpty()
                        && (canonical == prefix || pathIsWithin(canonical, prefix)
                            || pathIsWithin(prefix, canonical))) {
                        usage.prefixInUse = true;
                        usage.prefixUsers.append(QStringLiteral("PID %1 (%2)").arg(pid).arg(firstBase));
                        break;
                    }
                }
            }
        }

        const bool gameMatch = processMatchesGame(arguments, executable, cwd, canonicalGameDirs.value(0));
        bool anyGame = gameMatch;
        if (!anyGame) {
            for (int i = 1; i < canonicalGameDirs.size(); ++i) {
                if (processMatchesGame(arguments, executable, cwd, canonicalGameDirs.at(i))) {
                    anyGame = true;
                    break;
                }
            }
        }
        if (anyGame) {
            usage.gameInUse = true;
            usage.gameUsers.append(QStringLiteral("PID %1 (%2)").arg(pid).arg(firstBase));
        }
    }

    usage.steamPids.erase(std::unique(usage.steamPids.begin(), usage.steamPids.end()),
                          usage.steamPids.end());
    usage.steamNames.removeDuplicates();
    usage.prefixUsers.removeDuplicates();
    usage.gameUsers.removeDuplicates();
    return usage;
}

QString steamShutdownCommand(InstallKind kind)
{
    switch (kind) {
    case InstallKind::Flatpak:
        return QStringLiteral("flatpak run com.valvesoftware.Steam -shutdown");
    case InstallKind::Snap:
        return QStringLiteral("snap run steam -shutdown");
    case InstallKind::Native:
    default:
        return QStringLiteral("steam -shutdown");
    }
}

QString steamLaunchCommand(InstallKind kind)
{
    switch (kind) {
    case InstallKind::Flatpak:
        return QStringLiteral("flatpak run com.valvesoftware.Steam");
    case InstallKind::Snap:
        return QStringLiteral("snap run steam");
    case InstallKind::Native:
    default:
        return QStringLiteral("steam");
    }
}

bool startSteam(InstallKind kind, QString *error)
{
    QString program = QStringLiteral("steam");
    QStringList arguments;
    if (kind == InstallKind::Flatpak) {
        program = QStringLiteral("flatpak");
        arguments = {QStringLiteral("run"), QStringLiteral("com.valvesoftware.Steam")};
    } else if (kind == InstallKind::Snap) {
        program = QStringLiteral("snap");
        arguments = {QStringLiteral("run"), QStringLiteral("steam")};
    }
    if (!QProcess::startDetached(program, arguments)) {
        if (error)
            *error = QStringLiteral("Could not start Steam with '%1'").arg(program);
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool requestSteamShutdown(InstallKind kind, QString *error)
{
    QString program = QStringLiteral("steam");
    QStringList arguments{QStringLiteral("-shutdown")};
    if (kind == InstallKind::Flatpak) {
        program = QStringLiteral("flatpak");
        arguments = {QStringLiteral("run"), QStringLiteral("com.valvesoftware.Steam"),
                     QStringLiteral("-shutdown")};
    } else if (kind == InstallKind::Snap) {
        program = QStringLiteral("snap");
        arguments = {QStringLiteral("run"), QStringLiteral("steam"), QStringLiteral("-shutdown")};
    }

    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("Cannot start '%1': %2").arg(program, process.errorString());
        return false;
    }
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("Steam shutdown command did not finish in time; quit Steam manually");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("Steam shutdown command failed (exit %1): %2")
                         .arg(process.exitCode())
                         .arg(QString::fromUtf8(process.readAllStandardOutput()).trimmed());
        }
        return false;
    }
    return true;
}

} // namespace slm
