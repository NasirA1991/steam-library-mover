// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QPixmap>
#include <QVector>

class GameTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        CheckColumn,
        NameColumn,
        SizeColumn,
        DriveColumn,
        RuntimeColumn,
        LastPlayedColumn,
        StatusColumn,
        ColumnCount
    };
    enum class StatusFilter { All, Ready, Blocked, Warnings };
    enum class RuntimeFilter { All, Proton, Native, Auto };
    enum class SortMode { Name, SizeLargest, LastPlayed, Runtime, Drive, Status };
    enum Role { AppIdRole = Qt::UserRole + 1, GameIndexRole };

    explicit GameTableModel(QObject *parent = nullptr);

    void setScan(const slm::SteamScan &scan);
    void setLibraryFilter(const QString &libraryPath);
    void setSearchText(const QString &text);
    void setShowTools(bool show);
    void setStatusFilter(StatusFilter filter);
    void setRuntimeFilter(RuntimeFilter filter);
    void setSortMode(SortMode mode);
    void setSort(SortMode mode, bool ascending);
    SortMode sortMode() const { return m_sortMode; }
    bool sortAscending() const { return m_sortAscending; }
    void setArtwork(quint32 appId, const QString &path);
    void setCheckedAppIds(const QSet<quint32> &appIds);
    QVector<quint32> checkedAppIds() const;
    void applyFilters();

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;

    const slm::Game &gameAt(int row) const;
    int visibleCount() const { return m_visible.size(); }
    bool hasScan() const { return m_hasScan; }
    const slm::SteamScan &scan() const { return m_scan; }

signals:
    void checkStateChanged();

private:
    QString statusText(const slm::Game &game) const;
    QString tooltipFor(const slm::Game &game) const;
    QIcon iconFor(const slm::Game &game) const;

    slm::SteamScan m_scan;
    QVector<slm::Game> m_visible;
    mutable QHash<QString, QIcon> m_iconCache;
    QHash<quint32, QIcon> m_artworkIcons;
    QSet<quint32> m_checked;
    QString m_libraryFilter;
    QString m_searchText;
    bool m_showTools = false;
    bool m_hasScan = false;
    StatusFilter m_statusFilter = StatusFilter::All;
    RuntimeFilter m_runtimeFilter = RuntimeFilter::All;
    SortMode m_sortMode = SortMode::Name;
    bool m_sortAscending = true;
};
