// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QVector>

class QLabel;
class QProgressBar;
class QPushButton;

class MoveProgressDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MoveProgressDialog(int totalGames, QWidget *parent = nullptr);

    void setGame(int index, int total, const QString &name, const QString &from = {},
                 const QString &to = {}, const QString &artworkPath = {});
    void setProgress(const slm::MoveProgress &progress);
    void setQueueProgress(quint64 doneBytes, quint64 totalBytes);
    void setPaused(bool paused);
    void setQueueMessage(const QString &message);
    bool isPaused() const { return m_paused; }
    void finish();
    void setPauseEnabled(bool enabled);

signals:
    void pauseToggled(bool paused);
    void cancelRequested();

protected:
    void reject() override;

private:
    void updateStages(slm::Phase phase, quint64 doneBytes, quint64 totalBytes);

    bool m_paused = false;
    bool m_finished = false;
    QLabel *m_heading = nullptr;
    QLabel *m_subheading = nullptr;
    QLabel *m_cover = nullptr;
    QLabel *m_gameName = nullptr;
    QLabel *m_gameMeta = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_progressText = nullptr;
    QLabel *m_percent = nullptr;
    QLabel *m_speed = nullptr;
    QLabel *m_elapsed = nullptr;
    QLabel *m_remaining = nullptr;
    QLabel *m_detail = nullptr;
    QLabel *m_safety = nullptr;
    QLabel *m_queueText = nullptr;
    QLabel *m_queueMeta = nullptr;
    QVector<QLabel *> m_stageRows;
    QProgressBar *m_progress = nullptr;
    QProgressBar *m_queueProgress = nullptr;
    QElapsedTimer m_timer;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
};
