// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamemodel.h"

#include "uiformat.h"
#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QLocale>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QStyle>

#include <algorithm>

namespace {

QString relativePlayed(qint64 seconds)
{
    if (seconds <= 0)
        return QStringLiteral("Never");
    const QDate date = QDateTime::fromSecsSinceEpoch(seconds).date();
    const int days = date.daysTo(QDate::currentDate());
    if (days <= 0)
        return QStringLiteral("Today");
    if (days == 1)
        return QStringLiteral("Yesterday");
    if (days < 7)
        return QStringLiteral("%1 days ago").arg(days);
    if (days < 30)
        return QStringLiteral("%1 week%2 ago")
            .arg(days / 7)
            .arg(days / 7 == 1 ? QString() : QStringLiteral("s"));
    if (days < 365)
        return QStringLiteral("%1 month%2 ago")
            .arg(days / 30)
            .arg(days / 30 == 1 ? QString() : QStringLiteral("s"));
    return QStringLiteral("%1 year%2 ago")
        .arg(days / 365)
        .arg(days / 365 == 1 ? QString() : QStringLiteral("s"));
}

QString statusSortKey(const slm::Game &game)
{
    bool warning = false;
    for (const slm::Issue &issue : game.issues) {
        if (issue.isBlocker())
            return QStringLiteral("0");
        warning = true;
    }
    return warning ? QStringLiteral("1") : QStringLiteral("2");
}

} // namespace

GameTableModel::GameTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

void GameTableModel::setScan(const slm::SteamScan &scan)
{
    m_scan = scan;
    m_hasScan = true;
    m_checked.clear();
    m_iconCache.clear();
    m_artworkIcons.clear();
    applyFilters();
}

void GameTableModel::setLibraryFilter(const QString &libraryPath)
{
    if (m_libraryFilter == libraryPath)
        return;
    m_libraryFilter = libraryPath;
    applyFilters();
}

void GameTableModel::setSearchText(const QString &text)
{
    if (m_searchText == text)
        return;
    m_searchText = text;
    applyFilters();
}

void GameTableModel::setShowTools(bool show)
{
    if (m_showTools == show)
        return;
    m_showTools = show;
    applyFilters();
}

void GameTableModel::setStatusFilter(StatusFilter filter)
{
    if (m_statusFilter == filter)
        return;
    m_statusFilter = filter;
    applyFilters();
}

void GameTableModel::setRuntimeFilter(RuntimeFilter filter)
{
    if (m_runtimeFilter == filter)
        return;
    m_runtimeFilter = filter;
    applyFilters();
}

void GameTableModel::setSortMode(SortMode mode)
{
    if (m_sortMode == mode)
        return;
    m_sortMode = mode;
    switch (mode) {
    case SortMode::Name:
    case SortMode::Runtime:
    case SortMode::Drive:
    case SortMode::Status:
        m_sortAscending = true;
        break;
    case SortMode::SizeLargest:
    case SortMode::LastPlayed:
        m_sortAscending = false;
        break;
    }
    applyFilters();
}

void GameTableModel::setSort(SortMode mode, bool ascending)
{
    m_sortMode = mode;
    m_sortAscending = ascending;
    applyFilters();
}

void GameTableModel::setArtwork(quint32 appId, const QString &path)
{
    if (path.isEmpty())
        return;
    QPixmap pixmap(path);
    if (pixmap.isNull())
        return;
    m_artworkIcons.insert(appId, QIcon(pixmap));
    for (int row = 0; row < m_visible.size(); ++row) {
        if (m_visible.at(row).appId == appId) {
            const QModelIndex index = this->index(row, NameColumn);
            emit dataChanged(index, index, {Qt::DecorationRole, Qt::ToolTipRole});
        }
    }
}

void GameTableModel::setCheckedAppIds(const QSet<quint32> &appIds)
{
    if (m_checked == appIds)
        return;
    m_checked = appIds;
    if (!m_visible.isEmpty()) {
        emit dataChanged(index(0, CheckColumn), index(m_visible.size() - 1, CheckColumn),
                         {Qt::CheckStateRole});
    }
}

QVector<quint32> GameTableModel::checkedAppIds() const
{
    QVector<quint32> result;
    for (const slm::Game &game : m_visible) {
        if (m_checked.contains(game.appId))
            result.append(game.appId);
    }
    return result;
}

void GameTableModel::applyFilters()
{
    beginResetModel();
    m_visible.clear();
    const QString needle = m_searchText.trimmed();
    for (const slm::Game &game : m_scan.games) {
        if (!m_showTools
            && (game.category == slm::GameCategory::Tool
                || game.category == slm::GameCategory::SystemComponent
                || game.category == slm::GameCategory::Broken)) {
            continue;
        }
        if (!m_libraryFilter.isEmpty()) {
            const QString canonical = slm::canonicalOrAbsolute(game.libraryPath);
            if (canonical != slm::canonicalOrAbsolute(m_libraryFilter))
                continue;
        }
        if (!needle.isEmpty()) {
            const QString haystack = game.name + QLatin1Char(' ')
                + QString::number(game.appId) + QLatin1Char(' ') + game.installDir;
            if (!haystack.contains(needle, Qt::CaseInsensitive))
                continue;
        }
        bool blocker = false;
        bool warning = false;
        for (const slm::Issue &issue : game.issues) {
            blocker = blocker || issue.isBlocker();
            warning = warning || !issue.isBlocker();
        }
        if (m_statusFilter == StatusFilter::Ready && (blocker || warning))
            continue;
        if (m_statusFilter == StatusFilter::Blocked && !blocker)
            continue;
        if (m_statusFilter == StatusFilter::Warnings && (!warning || blocker))
            continue;
        if (m_runtimeFilter == RuntimeFilter::Proton
            && game.runtime != slm::RuntimeKind::Proton)
            continue;
        if (m_runtimeFilter == RuntimeFilter::Native
            && game.runtime != slm::RuntimeKind::Native)
            continue;
        if (m_runtimeFilter == RuntimeFilter::Auto
            && game.runtime != slm::RuntimeKind::Unknown)
            continue;
        m_visible.append(game);
    }
    const auto nameLess = [](const slm::Game &a, const slm::Game &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    };
    const auto less = [&](const slm::Game &a, const slm::Game &b) {
        switch (m_sortMode) {
        case SortMode::Name:
            return nameLess(a, b);
        case SortMode::SizeLargest:
            if (a.sizeOnDisk != b.sizeOnDisk)
                return a.sizeOnDisk < b.sizeOnDisk;
            return nameLess(a, b);
        case SortMode::LastPlayed:
            if (a.lastPlayed != b.lastPlayed)
                return a.lastPlayed < b.lastPlayed;
            return nameLess(a, b);
        case SortMode::Runtime:
            if (a.runtime != b.runtime)
                return static_cast<int>(a.runtime) < static_cast<int>(b.runtime);
            return nameLess(a, b);
        case SortMode::Drive: {
            const slm::Library *la = m_scan.findLibrary(a.libraryPath);
            const slm::Library *lb = m_scan.findLibrary(b.libraryPath);
            const QString na = la ? ui::driveDisplayName(*la) : a.libraryPath;
            const QString nb = lb ? ui::driveDisplayName(*lb) : b.libraryPath;
            if (na != nb)
                return QString::localeAwareCompare(na, nb) < 0;
            return nameLess(a, b);
        }
        case SortMode::Status: {
            const QString sa = statusSortKey(a);
            const QString sb = statusSortKey(b);
            if (sa != sb)
                return sa < sb;
            return nameLess(a, b);
        }
        }
        return false;
    };
    if (m_sortAscending)
        std::sort(m_visible.begin(), m_visible.end(), less);
    else
        std::sort(m_visible.begin(), m_visible.end(),
                  [&](const slm::Game &a, const slm::Game &b) { return less(b, a); });
    endResetModel();
}

int GameTableModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_visible.size();
}

int GameTableModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QString GameTableModel::statusText(const slm::Game &game) const
{
    for (const slm::Issue &issue : game.issues) {
        if (issue.isBlocker())
            return QStringLiteral("✕ Blocked");
    }
    for (const slm::Issue &issue : game.issues) {
        if (!issue.isBlocker())
            return QStringLiteral("⚠ Warning");
    }
    return {};
}

QString GameTableModel::tooltipFor(const slm::Game &game) const
{
    QStringList lines;
    lines << QStringLiteral("AppID: %1").arg(game.appId)
          << QStringLiteral("Install dir: %1").arg(game.installDir)
          << QStringLiteral("Library: %1").arg(game.libraryPath)
          << QStringLiteral("Runtime: %1").arg(slm::runtimeKindName(game.runtime));
    if (!game.protonVersion.isEmpty())
        lines << QStringLiteral("Proton: %1").arg(game.protonVersion);
    if (game.buildId != 0)
        lines << QStringLiteral("Build ID: %1").arg(game.buildId);
    lines << QStringLiteral("Manifest: %1").arg(game.manifestPath);
    lines << (game.sizeOnDisk == 0
                  ? QStringLiteral("Payload size: unknown (manifest has no SizeOnDisk)")
                  : QStringLiteral("Payload size: %1 (prefix and shadercache not included)")
                        .arg(slm::formatBytes(game.sizeOnDisk)));
    if (game.lastPlayed > 0) {
        lines << QStringLiteral("Last played: %1")
                     .arg(QLocale().toString(QDateTime::fromSecsSinceEpoch(game.lastPlayed),
                                             QLocale::LongFormat));
    }
    if (!game.issues.isEmpty()) {
        lines << QString();
        for (const slm::Issue &issue : game.issues) {
            lines << QStringLiteral("%1: %2")
                         .arg(issue.isBlocker() ? QStringLiteral("BLOCKER") : QStringLiteral("Warning"),
                              issue.message);
            if (!issue.detail.isEmpty())
                lines << QStringLiteral("  %1").arg(issue.detail);
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QIcon GameTableModel::iconFor(const slm::Game &game) const
{
    const auto artwork = m_artworkIcons.constFind(game.appId);
    if (artwork != m_artworkIcons.constEnd())
        return artwork.value();

    const QString key = QString::number(game.appId) + game.artworkPath;
    auto cached = m_iconCache.constFind(key);
    if (cached != m_iconCache.constEnd())
        return cached.value();
    QIcon icon;
    if (!game.artworkPath.isEmpty()) {
        QPixmap pixmap(game.artworkPath);
        if (!pixmap.isNull())
            icon = QIcon(pixmap);
    }
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("applications-games"),
                                QApplication::style()->standardIcon(QStyle::SP_FileIcon));
    m_iconCache.insert(key, icon);
    return icon;
}

QVariant GameTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return {};
    const slm::Game &game = m_visible.at(index.row());

    if (role == AppIdRole)
        return static_cast<qulonglong>(game.appId);
    if (role == GameIndexRole)
        return index.row();
    if (index.column() == CheckColumn) {
        if (role == Qt::CheckStateRole)
            return m_checked.contains(game.appId) ? Qt::Checked : Qt::Unchecked;
        if (role == Qt::TextAlignmentRole)
            return static_cast<int>(Qt::AlignCenter);
        if (role == Qt::ToolTipRole)
            return QStringLiteral("Select this game for a transfer");
        return {};
    }
    if (role == Qt::ToolTipRole) {
        if (index.column() == DriveColumn) {
            const slm::Library *library = m_scan.findLibrary(game.libraryPath);
            if (library)
                return ui::driveTooltip(*library);
        }
        return tooltipFor(game);
    }

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case NameColumn:
            return game.name;
        case SizeColumn:
            return game.sizeOnDisk == 0 ? QStringLiteral("unknown")
                                        : slm::formatBytes(game.sizeOnDisk);
        case DriveColumn: {
            const slm::Library *library = m_scan.findLibrary(game.libraryPath);
            return library ? ui::driveDisplayName(*library) : QFileInfo(game.libraryPath).fileName();
        }
        case RuntimeColumn:
            return slm::runtimeKindName(game.runtime);
        case LastPlayedColumn:
            return relativePlayed(game.lastPlayed);
        case StatusColumn:
            return statusText(game);
        default:
            return {};
        }
    }

    if (role == Qt::DecorationRole && index.column() == NameColumn)
        return iconFor(game);

    if (role == Qt::ForegroundRole) {
        bool blocker = false;
        bool warning = false;
        for (const slm::Issue &issue : game.issues) {
            blocker = blocker || issue.isBlocker();
            warning = warning || !issue.isBlocker();
        }
        if (blocker)
            return ui::isDarkTheme() ? QColor(255, 107, 107) : QColor(180, 30, 30);
        if (index.column() == StatusColumn && warning)
            return ui::isDarkTheme() ? QColor(255, 184, 77) : QColor(160, 100, 0);
        if (game.category != slm::GameCategory::Game)
            return QColor(120, 120, 120);
    }

    if (role == Qt::TextAlignmentRole && index.column() == SizeColumn)
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::FontRole && game.category != slm::GameCategory::Game) {
        QFont font;
        font.setItalic(true);
        return font;
    }
    return {};
}

QVariant GameTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::ToolTipRole && section == SizeColumn)
        return QStringLiteral("Game payload only; Proton prefix and shadercache are shown in details.");
    if (role != Qt::DisplayRole)
        return {};
    switch (section) {
    case CheckColumn:
        return QString();
    case NameColumn:
        return QStringLiteral("Game");
    case SizeColumn:
        return QStringLiteral("Payload");
    case DriveColumn:
        return QStringLiteral("Drive");
    case RuntimeColumn:
        return QStringLiteral("Runtime");
    case LastPlayedColumn:
        return QStringLiteral("Last played");
    case StatusColumn:
        return QStringLiteral("Status");
    default:
        return {};
    }
}

Qt::ItemFlags GameTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
    if (index.column() == CheckColumn)
        flags |= Qt::ItemIsUserCheckable;
    return flags;
}

bool GameTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size()
        || index.column() != CheckColumn || role != Qt::CheckStateRole)
        return QAbstractTableModel::setData(index, value, role);
    const quint32 appId = m_visible.at(index.row()).appId;
    if (value.toInt() == Qt::Checked)
        m_checked.insert(appId);
    else
        m_checked.remove(appId);
    emit dataChanged(index, index, {Qt::CheckStateRole});
    emit checkStateChanged();
    return true;
}

const slm::Game &GameTableModel::gameAt(int row) const
{
    static const slm::Game empty;
    if (row < 0 || row >= m_visible.size())
        return empty;
    return m_visible.at(row);
}
