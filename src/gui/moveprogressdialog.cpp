// SPDX-License-Identifier: GPL-3.0-or-later
#include "moveprogressdialog.h"

#include "theme.h"

#include <QApplication>

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString durationText(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("—");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString mutedColor()
{
    return ui::isDarkTheme() ? QStringLiteral("#aab4c0") : QStringLiteral("#66717d");
}

QString strongColor()
{
    return QApplication::palette().color(QPalette::Text).name();
}

QString accentColor()
{
    return QApplication::palette().color(QPalette::Highlight).name();
}

QString stageIndicator(const QString &symbol, bool done, bool active)
{
    const QString color = done ? QStringLiteral("#43b77a")
                               : active ? accentColor() : mutedColor();
    return QStringLiteral("<span style='color:%1'>%2</span>").arg(color, symbol);
}

struct StageInfo
{
    QString title;
    QString detail;
};

StageInfo stageInfo(int index, slm::Phase phase, quint64 doneBytes, quint64 totalBytes)
{
    StageInfo info;
    const bool done = phase == slm::Phase::Done;
    const bool copying = phase == slm::Phase::CopyingGame || phase == slm::Phase::CopyingAux;
    const bool verifying = phase == slm::Phase::Verifying || phase == slm::Phase::Syncing;
    const bool committing = phase == slm::Phase::WritingManifest || phase == slm::Phase::UpdatingVdf;
    const bool cleaning = phase == slm::Phase::RemovingSourceManifest
        || phase == slm::Phase::RemovingSourcePayload;
    switch (index) {
    case 0:
        info.title = QStringLiteral("Validate & prepare");
        info.detail = QStringLiteral("Preflight passed · Steam closed");
        break;
    case 1:
        info.title = QStringLiteral("Copy files");
        if (copying && totalBytes > 0) {
            const int percent = static_cast<int>((doneBytes * 100) / totalBytes);
            info.detail = QStringLiteral("%1 / %2 · %3%")
                              .arg(slm::formatBytes(doneBytes), slm::formatBytes(totalBytes))
                              .arg(qBound(0, percent, 100));
        } else if (done || verifying || committing || cleaning) {
            info.detail = QStringLiteral("Copied and verified");
        } else {
            info.detail = QStringLiteral("copy payload and auxiliary data");
        }
        break;
    case 2:
        info.title = QStringLiteral("Verify copied files");
        info.detail = verifying ? QStringLiteral("Scanning and checksumming copied data")
                                : done || committing || cleaning
                                      ? QStringLiteral("Copy verified")
                                      : QStringLiteral("types, sizes and checksums");
        break;
    case 3:
        info.title = QStringLiteral("Update Steam manifest");
        info.detail = committing ? QStringLiteral("atomic write with backup")
                                 : done || cleaning ? QStringLiteral("Manifest updated")
                                                    : QStringLiteral("only the moved game entry");
        break;
    case 4:
        info.title = QStringLiteral("Clean up source");
        info.detail = cleaning ? QStringLiteral("removing only after verification")
                               : done ? QStringLiteral("Source cleanup complete")
                                      : QStringLiteral("only after verification");
        break;
    default:
        break;
    }
    return info;
}

} // namespace

MoveProgressDialog::MoveProgressDialog(int totalGames, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Transferring games"));
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(760);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    m_heading = new QLabel(this);
    QFont headingFont = m_heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() + 2.0);
    m_heading->setFont(headingFont);
    layout->addWidget(m_heading);
    m_subheading = new QLabel(this);
    m_subheading->setStyleSheet(QStringLiteral("color: %1;").arg(mutedColor()));
    layout->addWidget(m_subheading);

    auto *gameRow = new QHBoxLayout();
    gameRow->setSpacing(12);
    m_cover = new QLabel(this);
    m_cover->setFixedSize(54, 72);
    m_cover->setAlignment(Qt::AlignCenter);
    m_cover->setStyleSheet(QStringLiteral(
        "background: palette(base); border: 1px solid palette(mid); border-radius: 3px; "
        "color: %1;").arg(mutedColor()));
    gameRow->addWidget(m_cover);
    auto *gameText = new QVBoxLayout();
    gameText->setSpacing(2);
    m_gameName = new QLabel(this);
    QFont gameFont = m_gameName->font();
    gameFont.setBold(true);
    gameFont.setPointSizeF(gameFont.pointSizeF() + 0.5);
    m_gameName->setFont(gameFont);
    m_gameMeta = new QLabel(this);
    m_gameMeta->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(mutedColor()));
    m_gameMeta->setWordWrap(true);
    gameText->addWidget(m_gameName);
    gameText->addWidget(m_gameMeta);
    gameRow->addLayout(gameText, 1);
    layout->addLayout(gameRow);

    auto *body = new QHBoxLayout();
    body->setSpacing(18);
    auto *stagePanel = new QWidget(this);
    stagePanel->setFixedWidth(218);
    auto *stageLayout = new QVBoxLayout(stagePanel);
    stageLayout->setContentsMargins(0, 2, 14, 0);
    stageLayout->setSpacing(12);
    for (int i = 0; i < 5; ++i) {
        auto *row = new QLabel(stagePanel);
        row->setTextFormat(Qt::RichText);
        row->setWordWrap(true);
        row->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_stageRows.append(row);
        stageLayout->addWidget(row);
    }
    stageLayout->addStretch();
    body->addWidget(stagePanel);

    auto *right = new QVBoxLayout();
    right->setSpacing(8);
    auto *progressRow = new QHBoxLayout();
    m_progressText = new QLabel(this);
    m_progressText->setStyleSheet(QStringLiteral("color: %1;").arg(mutedColor()));
    m_percent = new QLabel(this);
    m_percent->setStyleSheet(QStringLiteral("font-weight: 600;"));
    progressRow->addWidget(m_progressText, 1);
    progressRow->addWidget(m_percent);
    right->addLayout(progressRow);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    right->addWidget(m_progress);

    auto *metrics = new QHBoxLayout();
    metrics->setSpacing(7);
    const auto addMetric = [this, metrics](const QString &label, QLabel **valueOut) {
        auto *card = new QFrame(this);
        card->setObjectName(QStringLiteral("metricCard"));
        card->setStyleSheet(QStringLiteral(
            "QFrame#metricCard { background: palette(base); border: 1px solid palette(mid); "
            "border-radius: 4px; }"));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(8, 5, 8, 5);
        cardLayout->setSpacing(1);
        auto *kicker = new QLabel(label, card);
        kicker->setStyleSheet(QStringLiteral("color: %1; font-size: 8px;").arg(mutedColor()));
        auto *value = new QLabel(QStringLiteral("—"), card);
        value->setStyleSheet(QStringLiteral("font-weight: 600;"));
        cardLayout->addWidget(kicker);
        cardLayout->addWidget(value);
        metrics->addWidget(card, 1);
        *valueOut = value;
    };
    addMetric(QStringLiteral("SPEED"), &m_speed);
    addMetric(QStringLiteral("ELAPSED"), &m_elapsed);
    addMetric(QStringLiteral("REMAINING"), &m_remaining);
    right->addLayout(metrics);

    m_detail = new QLabel(this);
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setStyleSheet(QStringLiteral(
        "background: palette(base); border: 1px solid palette(mid); border-radius: 4px; "
        "padding: 6px 8px; color: %1;").arg(mutedColor()));
    right->addWidget(m_detail);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(mutedColor()));
    right->addWidget(m_status);

    m_safety = new QLabel(
        QStringLiteral("<b>Safe to cancel.</b> The source is not removed until copy, verification "
                       "and the manifest update succeed."),
        this);
    m_safety->setTextFormat(Qt::RichText);
    m_safety->setWordWrap(true);
    m_safety->setStyleSheet(QStringLiteral(
        "background: palette(base); border: 1px solid palette(mid); border-radius: 4px; "
        "padding: 6px 8px; color: %1; font-size: 10px;").arg(mutedColor()));
    right->addWidget(m_safety);
    right->addStretch();
    body->addLayout(right, 1);
    layout->addLayout(body, 1);

    auto *queueRow = new QHBoxLayout();
    queueRow->setSpacing(10);
    m_queueText = new QLabel(this);
    m_queueText->setStyleSheet(QStringLiteral("color: %1; font-size: 9px;").arg(mutedColor()));
    queueRow->addWidget(m_queueText);
    m_queueProgress = new QProgressBar(this);
    m_queueProgress->setRange(0, 100);
    m_queueProgress->setValue(0);
    m_queueProgress->setTextVisible(false);
    m_queueProgress->setFixedHeight(5);
    queueRow->addWidget(m_queueProgress, 1);
    m_queueMeta = new QLabel(this);
    m_queueMeta->setStyleSheet(QStringLiteral("color: %1; font-size: 9px;").arg(mutedColor()));
    queueRow->addWidget(m_queueMeta);
    layout->addLayout(queueRow);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch();
    m_pauseButton = new QPushButton(QStringLiteral("Pause"), this);
    m_pauseButton->setCheckable(true);
    connect(m_pauseButton, &QPushButton::toggled, this, [this](bool paused) {
        m_paused = paused;
        m_pauseButton->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
        if (!paused)
            m_status->setText(QStringLiteral("Resuming…"));
        emit pauseToggled(paused);
    });
    buttons->addWidget(m_pauseButton);
    m_cancelButton = new QPushButton(QStringLiteral("Cancel transfer"), this);
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
        m_cancelButton->setEnabled(false);
        m_cancelButton->setText(QStringLiteral("Cancelling…"));
        emit cancelRequested();
    });
    buttons->addWidget(m_cancelButton);
    layout->addLayout(buttons);

    setGame(0, totalGames, QString());
}

void MoveProgressDialog::setGame(int index, int total, const QString &name, const QString &from,
                                 const QString &to, const QString &artworkPath)
{
    m_heading->setText(total == 1 ? QStringLiteral("Transferring game")
                                  : QStringLiteral("Transferring %1 games").arg(total));
    m_subheading->setText(
        QStringLiteral("Game %1 of %2 · source remains untouched until commit")
            .arg(index + 1)
            .arg(total));
    m_gameName->setText(name.isEmpty() ? QStringLiteral("Preparing…") : name);
    m_gameMeta->setText(
        from.isEmpty() && to.isEmpty()
            ? QStringLiteral("Preparing the transfer plan…")
            : QStringLiteral("%1 → %2 · copy and verify").arg(from, to));
    m_cover->clear();
    if (!artworkPath.isEmpty()) {
        const QPixmap pixmap(artworkPath);
        if (!pixmap.isNull())
            m_cover->setPixmap(pixmap.scaled(54, 72, Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation));
    }
    if (m_cover->pixmap().isNull())
        m_cover->setText(QStringLiteral("—"));
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progressText->setText(QStringLiteral("Starting…"));
    m_percent->clear();
    m_speed->setText(QStringLiteral("—"));
    m_elapsed->setText(QStringLiteral("—"));
    m_remaining->setText(QStringLiteral("—"));
    m_detail->setText(QStringLiteral("Preparing…"));
    m_status->setText(QStringLiteral("Preparing the transfer plan…"));
    m_queueText->setText(QStringLiteral("QUEUE · GAME %1 OF %2").arg(index + 1).arg(total));
    m_timer.restart();
    updateStages(slm::Phase::Planned, 0, 0);
}

void MoveProgressDialog::setProgress(const slm::MoveProgress &progress)
{
    const bool busyPhase = progress.phase == slm::Phase::Verifying
        || progress.phase == slm::Phase::Syncing;
    updateStages(progress.phase, progress.bytesDone, progress.bytesTotal);
    if (busyPhase) {
        m_progress->setRange(0, 0);
        m_percent->clear();
        m_progressText->setText(progress.phase == slm::Phase::Verifying
                                    ? QStringLiteral("Verifying copied files…")
                                    : QStringLiteral("Flushing data to disk…"));
        setPauseEnabled(false);
        if (progress.phase == slm::Phase::Verifying) {
            m_detail->setText(
                QStringLiteral("Scanning the copied data and comparing it with the source. "
                               "The source is not removed until this succeeds."));
            m_status->setText(
                QStringLiteral("Full verification reads the copied payload; on a large game "
                               "this can take several minutes. Cancel is safe."));
        } else {
            m_detail->setText(QStringLiteral("Flushing copied data to the destination filesystem…"));
            m_status->clear();
        }
        if (!m_paused)
            m_status->setText(QStringLiteral("%1 — %2").arg(slm::phaseName(progress.phase),
                                                            progress.message));
        return;
    }

    m_progress->setRange(0, 100);
    if (progress.bytesTotal > 0) {
        const int percent = static_cast<int>((progress.bytesDone * 100) / progress.bytesTotal);
        m_progress->setValue(qBound(0, percent, 100));
        m_percent->setText(QStringLiteral("%1%").arg(qBound(0, percent, 100)));
        m_progressText->setText(QStringLiteral("%1 of %2")
                                    .arg(slm::formatBytes(progress.bytesDone),
                                         slm::formatBytes(progress.bytesTotal)));
    } else {
        m_percent->clear();
        m_progressText->setText(QStringLiteral("Preparing…"));
    }
    m_detail->setText(progress.currentFile.isEmpty()
                          ? QStringLiteral("Working…")
                          : QStringLiteral("Current file: %1").arg(progress.currentFile));
    if (m_timer.isValid() && progress.bytesDone > 0) {
        const qint64 elapsedMs = qMax<qint64>(1, m_timer.elapsed());
        const double seconds = elapsedMs / 1000.0;
        const double speed = progress.bytesDone / seconds;
        m_speed->setText(QStringLiteral("%1/s").arg(
            slm::formatBytes(static_cast<quint64>(speed))));
        m_elapsed->setText(durationText(elapsedMs / 1000));
        if (progress.bytesDone < progress.bytesTotal && speed > 1.0) {
            const qint64 remaining =
                static_cast<qint64>((progress.bytesTotal - progress.bytesDone) / speed);
            m_remaining->setText(QStringLiteral("~%1").arg(durationText(remaining)));
        } else {
            m_remaining->setText(QStringLiteral("—"));
        }
    }
    if (!m_paused)
        m_status->setText(QStringLiteral("%1 — %2").arg(slm::phaseName(progress.phase),
                                                        progress.message));
}

void MoveProgressDialog::setQueueProgress(quint64 doneBytes, quint64 totalBytes)
{
    if (totalBytes == 0) {
        m_queueMeta->setText(QStringLiteral("—"));
        m_queueProgress->setValue(0);
        return;
    }
    const int percent = static_cast<int>((doneBytes * 100) / totalBytes);
    m_queueProgress->setValue(qBound(0, percent, 100));
    m_queueMeta->setText(QStringLiteral("%1 / %2 · %3%")
                             .arg(slm::formatBytes(doneBytes), slm::formatBytes(totalBytes))
                             .arg(qBound(0, percent, 100)));
}

void MoveProgressDialog::updateStages(slm::Phase phase, quint64 doneBytes, quint64 totalBytes)
{
    const bool done = phase == slm::Phase::Done;
    const bool copying = phase == slm::Phase::CopyingGame || phase == slm::Phase::CopyingAux;
    const bool verifying = phase == slm::Phase::Verifying || phase == slm::Phase::Syncing;
    const bool committing = phase == slm::Phase::WritingManifest || phase == slm::Phase::UpdatingVdf;
    const bool cleaning = phase == slm::Phase::RemovingSourceManifest
        || phase == slm::Phase::RemovingSourcePayload;
    const QVector<QPair<bool, bool>> states = {
        {done || copying || verifying || committing || cleaning, phase == slm::Phase::Planned},
        {done || verifying || committing || cleaning, copying},
        {done || committing || cleaning, verifying},
        {done || cleaning, committing},
        {done, cleaning},
    };
    for (int i = 0; i < m_stageRows.size(); ++i) {
        const StageInfo info = stageInfo(i, phase, doneBytes, totalBytes);
        const bool completed = states.at(i).first;
        const bool active = states.at(i).second;
        const QString symbol = completed ? QStringLiteral("✓") : active ? QStringLiteral("●")
                                                                        : QStringLiteral("○");
        const QString titleColor = (completed || active) ? strongColor() : mutedColor();
        m_stageRows[i]->setText(
            QStringLiteral("%1 <b style='color:%2'>%3</b><br>"
                           "<span style='color:%4; font-size:9px'>%5</span>")
                .arg(stageIndicator(symbol, completed, active), titleColor, info.title,
                     mutedColor(), info.detail));
    }
}

void MoveProgressDialog::setPaused(bool paused)
{
    const QSignalBlocker blocker(m_pauseButton);
    m_paused = paused;
    m_pauseButton->setChecked(paused);
    m_pauseButton->setText(paused ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    m_status->setText(paused ? QStringLiteral("Paused — source and destination are untouched.")
                             : QStringLiteral("Resuming…"));
    if (paused) {
        m_speed->setText(QStringLiteral("—"));
        m_remaining->setText(QStringLiteral("—"));
    } else {
        m_timer.restart();
    }
}

void MoveProgressDialog::setQueueMessage(const QString &message)
{
    m_status->setText(message);
}

void MoveProgressDialog::setPauseEnabled(bool enabled)
{
    m_pauseButton->setEnabled(enabled);
    if (!enabled && m_pauseButton->isChecked()) {
        QSignalBlocker blocker(m_pauseButton);
        m_pauseButton->setChecked(false);
        m_pauseButton->setText(QStringLiteral("Pause"));
        m_paused = false;
    }
}

void MoveProgressDialog::finish()
{
    m_finished = true;
    m_pauseButton->setEnabled(false);
    m_cancelButton->setEnabled(false);
    accept();
}

void MoveProgressDialog::reject()
{
    if (!m_finished) {
        m_cancelButton->click();
        return;
    }
    QDialog::reject();
}
