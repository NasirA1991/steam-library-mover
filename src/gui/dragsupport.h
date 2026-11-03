// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QListWidget>
#include <QTableView>

class GameTableView : public QTableView
{
    Q_OBJECT
public:
    explicit GameTableView(QWidget *parent = nullptr);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

class LibraryListWidget : public QListWidget
{
    Q_OBJECT
public:
    explicit LibraryListWidget(QWidget *parent = nullptr);

signals:
    void gamesDroppedOnLibrary(const QString &libraryPath, const QList<quint32> &appIds);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};
