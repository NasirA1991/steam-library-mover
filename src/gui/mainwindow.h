// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/mountinfo.h"
#include "core/types.h"

#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <memory>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressDialog;
class QPushButton;
class QTimer;
class QThread;
class QToolBar;
class QToolButton;
class GameTableModel;
class GameTableView;
class LibraryListWidget;
class MoveProgressDialog;
class SleepInhibitor;
class ScanThread;
class PlanThread;
class MoveThread;
class ArtworkProvider;
class SizeThread;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    bool isScanning() const { return m_scanInProgress || !m_scanThread.isNull(); }

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void rescan();
    void onRootChanged(int index);
    void onScanReady(const slm::SteamScan &scan, const QString &error);
    void onLibrarySelected();
    void onSelectionChanged();
    void onSearchChanged(const QString &text);
    void onShowToolsToggled(bool show);
    void onGamesDroppedOnLibrary(const QString &libraryPath, const QList<quint32> &appIds);
    void quitSteam();
    void reviewMove();
    void onPlanReady(const slm::MovePlan &plan, const QString &error);
    void onMoveProgress(const slm::MoveProgress &progress);
    void onMoveFinished(const slm::MoveResult &result);
    void onPauseToggled(bool paused);
    void onCancelQueueRequested();
    void showLibraryHealth();
    void showDoctor();
    void showRecovery();
    void showSettings();
    void showAbout();
    void updateSteamBanner();
    void applySteamStatus(bool running);
    void refreshTheme();
    void showGameContextMenu(const QPoint &pos);
    void onHeaderClicked(int section);
    void updateFilterCounts();
    void updateSortIndicator();
    void updateDestinationProjections();
    void applyDensity();
    QString reviewBlockReason() const;
    quint64 selectedPayloadBytes() const;
    void onArtworkReady(quint32 appId, const QString &path);
    void onSizesReady(quint32 appId, quint64 prefixBytes, quint64 shaderBytes,
                      quint64 workshopBytes);

private:
    void buildUi();
    void applyCachedScan(const slm::SteamScan &scan);
    void populateRoots();
    void populateLibraries();
    void populateDestinations();
    void updateDetails();
    void updateSummary();
    void startScan();
    void beginPlanning(const QVector<quint32> &appIds, const QString &destination);
    void planNextGame();
    void showBatchReview();
    void startMoveQueue(const QVector<slm::MovePlan> &plans);
    void startNextMove();
    void finishMoveQueue(const QString &stopReason);
    bool refreshRemainingSnapshots(QString *error = nullptr);
    void selectGamesByIds(const QList<quint32> &appIds);
    QVector<quint32> selectedAppIds() const;
    QString selectedDestination() const;
    const slm::Game *selectedGame() const;
    const slm::Library *libraryFor(const QString &path) const;
    slm::MoveOptions optionsFromSettings() const;
    void promptUnfinishedOperations();
    void setBannerMessage(const QString &text, const QString &accent, bool compact = false);
    void requestArtworkForScan();

    GameTableModel *m_model = nullptr;
    GameTableView *m_table = nullptr;
    LibraryListWidget *m_libraryList = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_heading = nullptr;
    QLabel *m_headingSubtitle = nullptr;
    QLabel *m_banner = nullptr;
    QLabel *m_details = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_selectionCount = nullptr;
    QLabel *m_sourceSummary = nullptr;
    QLabel *m_projectionLabel = nullptr;
    QLabel *m_transferEyebrow = nullptr;
    QToolButton *m_clearSelection = nullptr;
    QWidget *m_detailsPanel = nullptr;
    QLabel *m_detailsSummary = nullptr;
    QToolButton *m_detailsToggle = nullptr;
    QComboBox *m_destination = nullptr;
    QComboBox *m_rootCombo = nullptr;
    QComboBox *m_statusFilter = nullptr;
    QComboBox *m_runtimeFilter = nullptr;
    QComboBox *m_sortMode = nullptr;
    QPushButton *m_reviewButton = nullptr;
    QAction *m_rescanAction = nullptr;
    QAction *m_quitSteamAction = nullptr;
    QAction *m_showToolsAction = nullptr;
    QToolBar *m_toolbar = nullptr;
    QTimer *m_statusTimer = nullptr;
    QThread *m_processThread = nullptr;
    QThread *m_shutdownThread = nullptr;

    QVector<slm::SteamRoot> m_roots;
    slm::SteamScan m_scan;
    slm::MountTable m_mounts;
    QString m_libraryFilter;

    QPointer<ScanThread> m_scanThread;
    QPointer<PlanThread> m_planThread;
    QPointer<MoveThread> m_moveThread;
    QPointer<QProgressDialog> m_planProgress;
    QPointer<MoveProgressDialog> m_moveProgress;
    ArtworkProvider *m_artwork = nullptr;
    std::unique_ptr<SleepInhibitor> m_sleepInhibitor;

    QVector<quint32> m_planAppIds;
    QVector<slm::MovePlan> m_batchPlans;
    QVector<slm::MoveResult> m_batchResults;
    QMap<QString, quint64> m_batchReserved;
    QString m_batchDestination;
    slm::MoveOptions m_batchOptions;
    int m_planIndex = 0;
    int m_moveIndex = 0;
    bool m_queuePaused = false;
    quint64 m_queueTotalBytes = 0;
    quint64 m_queueBaseBytes = 0;
    bool m_queueCancelled = false;
    bool m_queueFinished = false;
    QString m_batchStopReason;

    struct GameSizes
    {
        quint64 prefix = 0;
        quint64 shader = 0;
        quint64 workshop = 0;
        bool ready = false;
    };
    QHash<quint32, GameSizes> m_sizeCache;
    QSet<quint32> m_sizePending;
    QList<QPointer<SizeThread>> m_sizeThreads;
    bool m_scanInProgress = false;
    bool m_steamRunning = false;
    bool m_scanIsCached = false;
    bool m_firstScan = true;
};
