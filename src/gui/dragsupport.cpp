// SPDX-License-Identifier: GPL-3.0-or-later
#include "dragsupport.h"

#include "gamemodel.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>

namespace {
constexpr auto kAppIdsMime = "application/x-steam-library-mover-appids";
}

GameTableView::GameTableView(QWidget *parent)
    : QTableView(parent)
{
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
}

void GameTableView::startDrag(Qt::DropActions supportedActions)
{
    Q_UNUSED(supportedActions);
    const QModelIndexList rows = selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;

    QStringList ids;
    QStringList names;
    for (const QModelIndex &index : rows) {
        const QVariant appId = index.data(GameTableModel::AppIdRole);
        if (!appId.isValid() || appId.toUInt() == 0)
            continue;
        ids << QString::number(appId.toUInt());
        const QString name = index.data(Qt::DisplayRole).toString();
        if (!name.isEmpty())
            names << name;
    }
    if (ids.isEmpty())
        return;

    auto *mime = new QMimeData();
    mime->setData(kAppIdsMime, ids.join(QLatin1Char(',')).toUtf8());
    mime->setText(QStringLiteral("Steam games: %1").arg(names.join(QStringLiteral(", "))));
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction);
}

LibraryListWidget::LibraryListWidget(QWidget *parent)
    : QListWidget(parent)
{
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DropOnly);
    setDefaultDropAction(Qt::CopyAction);
}

void LibraryListWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(kAppIdsMime))
        event->acceptProposedAction();
    else
        event->ignore();
}

void LibraryListWidget::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(kAppIdsMime)) {
        event->ignore();
        return;
    }
    const QListWidgetItem *item = itemAt(event->position().toPoint());
    if (item && !item->data(Qt::UserRole).toString().isEmpty())
        event->acceptProposedAction();
    else
        event->ignore();
}

void LibraryListWidget::dropEvent(QDropEvent *event)
{
    const QListWidgetItem *item = itemAt(event->position().toPoint());
    const QString path = item ? item->data(Qt::UserRole).toString() : QString();
    if (path.isEmpty() || !event->mimeData()->hasFormat(kAppIdsMime)) {
        event->ignore();
        return;
    }
    QList<quint32> appIds;
    const QStringList parts = QString::fromUtf8(event->mimeData()->data(kAppIdsMime))
                                  .split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        const quint32 appId = part.toUInt(&ok);
        if (ok && appId != 0)
            appIds.append(appId);
    }
    if (appIds.isEmpty()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    emit gamesDroppedOnLibrary(path, appIds);
}
