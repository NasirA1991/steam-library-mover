// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/executor.h"
#include "core/filetree.h"
#include "core/library.h"
#include "core/mountinfo.h"
#include "core/planner.h"
#include "core/scanner.h"
#include "core/types.h"

#include <QThread>
#include <QFileInfo>

#include <atomic>

class ScanThread : public QThread
{
    Q_OBJECT
public:
    explicit ScanThread(slm::SteamRoot root, QObject *parent = nullptr)
        : QThread(parent)
        , m_root(std::move(root))
    {
    }

signals:
    void scanReady(slm::SteamScan scan, QString error);

protected:
    void run() override
    {
        QString mountError;
        const slm::MountTable mounts = slm::MountTable::load(&mountError);
        QString error;
        const slm::SteamScan scan = slm::scanSteam(m_root, mounts, &error);
        if (error.isEmpty() && !mountError.isEmpty())
            error = mountError;
        emit scanReady(scan, error);
    }

private:
    slm::SteamRoot m_root;
};

class PlanThread : public QThread
{
    Q_OBJECT
public:
    PlanThread(slm::SteamScan scan, quint32 appId, QString destination, slm::MoveOptions options,
               QMap<QString, quint64> reserved = {}, QObject *parent = nullptr)
        : QThread(parent)
        , m_scan(std::move(scan))
        , m_appId(appId)
        , m_destination(std::move(destination))
        , m_options(options)
        , m_reserved(std::move(reserved))
    {
    }

signals:
    void planReady(slm::MovePlan plan, QString error);

protected:
    void run() override
    {
        const slm::MountTable mounts = slm::MountTable::fromMounts(m_scan.mounts);
        QString error;
        const slm::MovePlan plan = slm::MovePlanner::plan(
            m_scan, m_appId, m_destination, m_options, mounts, &error, m_reserved);
        emit planReady(plan, error);
    }

private:
    slm::SteamScan m_scan;
    quint32 m_appId;
    QString m_destination;
    slm::MoveOptions m_options;
    QMap<QString, quint64> m_reserved;
};

class SizeThread : public QThread
{
    Q_OBJECT
public:
    SizeThread(slm::Library library, slm::Game game, QObject *parent = nullptr)
        : QThread(parent)
        , m_library(std::move(library))
        , m_game(std::move(game))
    {
    }

signals:
    void sizesReady(quint32 appId, quint64 prefixBytes, quint64 shaderBytes,
                    quint64 workshopBytes);

protected:
    void run() override
    {
        auto sizeOf = [](const QString &path) -> quint64 {
            if (!QFileInfo::exists(path))
                return 0;
            const slm::FileTree tree = slm::FileTree::scan(path);
            return tree.ok ? tree.uniqueBytes : 0;
        };

        bool prefixExists = false;
        const QString prefix = slm::resolvePrefixPath(m_library, m_game.appId, &prefixExists);
        const quint64 prefixBytes = prefixExists ? sizeOf(prefix) : 0;
        const QString shader = m_library.steamappsPath() + QStringLiteral("/shadercache/")
            + QString::number(m_game.appId);
        const quint64 shaderBytes = sizeOf(shader);
        const QString workshop = m_library.steamappsPath() + QStringLiteral("/workshop/content/")
            + QString::number(m_game.appId);
        quint64 workshopBytes = sizeOf(workshop);
        const QFileInfo workshopManifest(
            m_library.steamappsPath() + QStringLiteral("/workshop/appworkshop_%1.acf")
                                            .arg(m_game.appId));
        if (workshopManifest.exists())
            workshopBytes += static_cast<quint64>(workshopManifest.size());
        emit sizesReady(m_game.appId, prefixBytes, shaderBytes, workshopBytes);
    }

private:
    slm::Library m_library;
    slm::Game m_game;
};

class MoveThread : public QThread
{
    Q_OBJECT
public:
    MoveThread(slm::MovePlan plan, slm::MountTable mounts, QObject *parent = nullptr)
        : QThread(parent)
        , m_plan(std::move(plan))
        , m_mounts(std::move(mounts))
    {
    }

    void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }
    void pause() { m_paused.store(true, std::memory_order_relaxed); }
    void resume() { m_paused.store(false, std::memory_order_relaxed); }
    bool isPaused() const { return m_paused.load(std::memory_order_relaxed); }

signals:
    void progressChanged(slm::MoveProgress progress);
    void moveFinished(slm::MoveResult result);

protected:
    void run() override
    {
        slm::MoveExecutor executor;
        const slm::MoveResult result = executor.execute(
            m_plan, m_mounts,
            [&](const slm::MoveProgress &progress) { emit progressChanged(progress); },
            [&]() { return m_cancelled.load(std::memory_order_relaxed); },
            [&]() { return m_paused.load(std::memory_order_relaxed); });
        emit moveFinished(result);
    }

private:
    slm::MovePlan m_plan;
    slm::MountTable m_mounts;
    std::atomic_bool m_cancelled{false};
    std::atomic_bool m_paused{false};
};
