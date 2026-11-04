// SPDX-License-Identifier: GPL-3.0-or-later
#include "mainwindow.h"

#include "core/doctor.h"
#include "core/executor.h"
#include "core/journal.h"
#include "core/library.h"
#include "core/mountinfo.h"
#include "core/planner.h"
#include "core/process_check.h"
#include "core/scancache.h"
#include "core/scanner.h"
#include "core/types.h"
#include "artworkprovider.h"
#include "core/filetree.h"
#include "core/health.h"
#include "desktopnotification.h"
#include "dragsupport.h"
#include "gamemodel.h"
#include "librarydelegate.h"
#include "moveprogressdialog.h"
#include "sleepinhibitor.h"
#include "theme.h"
#include "uiformat.h"
#include "workers.h"

#include <QAction>
#include <QApplication>
#include <algorithm>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QLocale>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QSignalBlocker>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QSplitter>
#include <QStatusBar>
#include <QUrl>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>

namespace {

void setMutedLabel(QLabel *label)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText,
                     ui::isDarkTheme() ? QColor(QStringLiteral("#9aa4af"))
                                       : QColor(QStringLiteral("#6b7480")));
    label->setPalette(palette);
}

QString compatDataDescription(const slm::Library &library)
{
    switch (library.compatdata.state) {
    case slm::CompatDataState::Missing:
        return QStringLiteral("missing");
    case slm::CompatDataState::Directory:
        return QStringLiteral("real directory");
    case slm::CompatDataState::Symlink:
        return QStringLiteral("symlink → %1").arg(library.compatdata.target);
    case slm::CompatDataState::BrokenSymlink:
        return QStringLiteral("broken symlink");
    }
    return QStringLiteral("unknown");
}

QString compatDataDoctorCell(const slm::Library &library)
{
    if (!library.mount.isWindowsCompatible())
        return compatDataDescription(library);
    switch (library.compatdata.state) {
    case slm::CompatDataState::Symlink:
        if (library.compatdata.targetOnWindowsFs)
            return QStringLiteral("⚠ target on Windows FS");
        return QStringLiteral("✔ configured (shared symlink)");
    case slm::CompatDataState::Missing:
        return QStringLiteral("⚠ not configured");
    case slm::CompatDataState::BrokenSymlink:
        return QStringLiteral("⛔ broken symlink");
    case slm::CompatDataState::Directory:
        return QStringLiteral("⚠ real directory on Windows FS");
    }
    return compatDataDescription(library);
}

QString issueLines(const QVector<slm::Issue> &issues)
{
    QStringList lines;
    for (const slm::Issue &issue : issues) {
        lines << QStringLiteral("%1 — %2")
                     .arg(issue.isBlocker() ? QStringLiteral("BLOCKER") : QStringLiteral("Warning"),
                          issue.message);
        if (!issue.detail.isEmpty())
            lines << QStringLiteral("    %1").arg(issue.detail);
    }
    return lines.join(QLatin1Char('\n'));
}

bool hasNtfsWarning(const slm::MovePlan &plan)
{
    for (const slm::Issue &issue : plan.issues) {
        if (issue.code == QStringLiteral("windows-filesystem")
            || issue.code == QStringLiteral("ntfs-exec-bits"))
            return true;
    }
    return false;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    buildUi();
    populateRoots();
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(5000);
    connect(m_statusTimer, &QTimer::timeout, this, &MainWindow::updateSteamBanner);
    m_statusTimer->start();
    rescan();
}

MainWindow::~MainWindow()
{
    // QApplication can destroy the window without a closeEvent (for example on
    // session logout or app.quit()). Never let a QThread child be destroyed
    // while it is still running.
    if (m_moveThread) {
        m_moveThread->cancel();
        m_moveThread->wait();
    }
    if (m_scanThread)
        m_scanThread->wait();
    if (m_planThread)
        m_planThread->wait();
    for (const QPointer<SizeThread> &thread : m_sizeThreads) {
        if (thread)
            thread->wait();
    }
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("Steam Library Mover"));
    resize(1280, 820);
    setMinimumSize(1100, 720);

    auto *fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    m_rescanAction = fileMenu->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                         QStringLiteral("&Rescan"), this, &MainWindow::rescan);
    m_rescanAction->setShortcut(QKeySequence::Refresh);
    fileMenu->addSeparator();
    auto *quitAction = fileMenu->addAction(QStringLiteral("&Quit"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    auto *toolsMenu = menuBar()->addMenu(QStringLiteral("&Tools"));
    toolsMenu->addAction(QStringLiteral("Library Health…"), this, &MainWindow::showLibraryHealth);
    toolsMenu->addAction(QStringLiteral("NTFS Doctor…"), this, &MainWindow::showDoctor);
    toolsMenu->addAction(QStringLiteral("Recovery…"), this, &MainWindow::showRecovery);
    toolsMenu->addSeparator();
    toolsMenu->addAction(QStringLiteral("Settings…"), this, &MainWindow::showSettings);

    auto *helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(QStringLiteral("&About"), this, &MainWindow::showAbout);

    m_toolbar = addToolBar(QStringLiteral("Main"));
    m_toolbar->setMovable(false);
    m_toolbar->addAction(m_rescanAction);
    m_quitSteamAction = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("application-exit")), QStringLiteral("Quit Steam"), this,
        &MainWindow::quitSteam);
    m_showToolsAction = m_toolbar->addAction(QStringLiteral("Show tools"));
    m_showToolsAction->setCheckable(true);
    connect(m_showToolsAction, &QAction::toggled, this, &MainWindow::onShowToolsToggled);
    m_toolbar->addSeparator();
    m_toolbar->addWidget(new QLabel(QStringLiteral("Steam installation: ")));
    m_rootCombo = new QComboBox(m_toolbar);
    m_rootCombo->setMinimumWidth(280);
    m_rootCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_toolbar->addWidget(m_rootCombo);
    connect(m_rootCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onRootChanged);
    m_toolbar->addSeparator();
    m_toolbar->addAction(QStringLiteral("Library Health…"), this, &MainWindow::showLibraryHealth);
    m_toolbar->addAction(QStringLiteral("NTFS Doctor…"), this, &MainWindow::showDoctor);

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(12, 10, 12, 10);
    outer->setSpacing(9);

    m_heading = new QLabel(QStringLiteral("Steam Library Mover"), central);
    QFont headingFont = m_heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + 8.0);
    headingFont.setBold(true);
    m_heading->setFont(headingFont);
    m_headingSubtitle = new QLabel(
        QStringLiteral("Move installed games between libraries safely, with verification and rollback."),
        central);
    const auto applySubtitleColor = [this]() {
        QPalette subtitlePalette = m_headingSubtitle->palette();
        subtitlePalette.setColor(QPalette::WindowText,
                                 ui::isDarkTheme() ? QColor(QStringLiteral("#9aa4af"))
                                                   : QColor(QStringLiteral("#6b7480")));
        m_headingSubtitle->setPalette(subtitlePalette);
    };
    applySubtitleColor();
    auto *headingLayout = new QVBoxLayout();
    headingLayout->setSpacing(1);
    headingLayout->addWidget(m_heading);
    headingLayout->addWidget(m_headingSubtitle);
    outer->addLayout(headingLayout);

    m_banner = new QLabel(central);
    m_banner->setWordWrap(true);
    m_banner->setTextFormat(Qt::PlainText);
    m_banner->setContentsMargins(12, 8, 12, 8);
    setBannerMessage(QStringLiteral("Checking Steam…"), QStringLiteral("#2d6cdf"));
    outer->addWidget(m_banner);

    const QString mutedText = ui::isDarkTheme() ? QStringLiteral("#aab4c0")
                                                 : QStringLiteral("#66717d");
    central->setStyleSheet(QStringLiteral(
        "QTableView#gameTable { border: 1px solid palette(mid); border-radius: 6px; padding: 2px; }"
        "QTableView#gameTable QHeaderView::section { background: palette(window); border: none; "
        "border-bottom: 1px solid palette(mid); padding: 7px 8px; font-weight: 600; }"
        "QListWidget#libraryList { border: 1px solid palette(mid); border-radius: 6px; padding: 4px; }"
        "QListWidget#libraryList::item { padding: 7px 5px; border-radius: 4px; }"
        "QListWidget#libraryList::item:selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QListWidget#libraryList::item:selected:!active { background: palette(midlight); color: palette(text); }"
        "QLineEdit#search { border: 1px solid palette(mid); border-radius: 6px; padding: 7px 9px; }"
        "QPushButton#reviewButton { padding: 8px 14px; font-weight: 600; }"
        "QWidget#detailsPanel { background: palette(base); border: 1px solid palette(mid); border-radius: 6px; }"
        "QToolButton#detailsToggle { border: none; color: palette(link); padding: 2px 4px; }"
        "QToolButton#detailsToggle:hover { background: palette(midlight); border-radius: 3px; }"
        "QFrame#transferStrip { background: palette(base); border: 1px solid palette(mid); border-radius: 6px; }"
        "QLabel#transferEyebrow { color: %1; font-size: 9px; font-weight: 600; letter-spacing: 1px; }"
        "QLabel#transferSummary { font-size: 13px; font-weight: 600; }"
        "QLabel#transferSource { color: %1; font-size: 10px; }"
        "QToolButton#clearSelection { border: none; color: palette(link); padding: 4px 6px; }"
        "QToolButton#clearSelection:hover { background: palette(midlight); border-radius: 3px; }")
                             .arg(mutedText));

    auto *splitter = new QSplitter(Qt::Horizontal, central);
    outer->addWidget(splitter, 1);

    auto *left = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 8, 0);
    auto *librariesLabel = new QLabel(QStringLiteral("<b>Libraries</b>"), left);
    leftLayout->addWidget(librariesLabel);
    m_libraryList = new LibraryListWidget(left);
    m_libraryList->setObjectName(QStringLiteral("libraryList"));
    m_libraryList->setItemDelegate(new LibraryItemDelegate(m_libraryList));
    m_libraryList->setWordWrap(true);
    m_libraryList->setAlternatingRowColors(false);
    m_libraryList->setIconSize(QSize(26, 26));
    m_libraryList->setSpacing(2);
    leftLayout->addWidget(m_libraryList, 1);
    connect(m_libraryList, &QListWidget::itemSelectionChanged, this,
            &MainWindow::onLibrarySelected);
    connect(m_libraryList, &LibraryListWidget::gamesDroppedOnLibrary, this,
            &MainWindow::onGamesDroppedOnLibrary);
    splitter->addWidget(left);

    auto *right = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    m_search = new QLineEdit(right);
    m_search->setObjectName(QStringLiteral("search"));
    m_search->setPlaceholderText(QStringLiteral("Search games by name, appid or install directory…"));
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
    rightLayout->addWidget(m_search);

    auto *filterRow = new QHBoxLayout();
    filterRow->setSpacing(6);
    m_statusFilter = new QComboBox(right);
    m_statusFilter->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_statusFilter->addItems({QStringLiteral("All statuses"), QStringLiteral("Ready"),
                              QStringLiteral("Blocked"), QStringLiteral("Warnings")});
    m_runtimeFilter = new QComboBox(right);
    m_runtimeFilter->addItems({QStringLiteral("All runtimes"), QStringLiteral("Proton"),
                               QStringLiteral("Native"), QStringLiteral("Unknown")});
    m_sortMode = new QComboBox(right);
    m_sortMode->addItem(QStringLiteral("Sort: name"), static_cast<int>(GameTableModel::SortMode::Name));
    m_sortMode->addItem(QStringLiteral("Sort: largest first"),
                        static_cast<int>(GameTableModel::SortMode::SizeLargest));
    m_sortMode->addItem(QStringLiteral("Sort: recently played"),
                        static_cast<int>(GameTableModel::SortMode::LastPlayed));
    m_sortMode->addItem(QStringLiteral("Sort: runtime"),
                        static_cast<int>(GameTableModel::SortMode::Runtime));
    m_sortMode->addItem(QStringLiteral("Sort: drive"),
                        static_cast<int>(GameTableModel::SortMode::Drive));
    m_sortMode->addItem(QStringLiteral("Sort: status"),
                        static_cast<int>(GameTableModel::SortMode::Status));
    filterRow->addWidget(m_statusFilter);
    filterRow->addWidget(m_runtimeFilter);
    filterRow->addWidget(m_sortMode);
    filterRow->addStretch();
    m_selectionCount = new QLabel(right);
    setMutedLabel(m_selectionCount);
    filterRow->addWidget(m_selectionCount);
    rightLayout->addLayout(filterRow);
    connect(m_statusFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_model->setStatusFilter(static_cast<GameTableModel::StatusFilter>(index));
        onSelectionChanged();
    });
    connect(m_runtimeFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_model->setRuntimeFilter(static_cast<GameTableModel::RuntimeFilter>(index));
        onSelectionChanged();
    });
    connect(m_sortMode, &QComboBox::currentIndexChanged, this, [this](int) {
        const auto mode = static_cast<GameTableModel::SortMode>(m_sortMode->currentData().toInt());
        m_model->setSortMode(mode);
        updateSortIndicator();
        onSelectionChanged();
    });

    m_table = new GameTableView(right);
    m_table->setObjectName(QStringLiteral("gameTable"));
    m_model = new GameTableModel(this);
    m_table->setModel(m_model);
    connect(m_model, &GameTableModel::checkStateChanged, this, [this]() {
        updateDestinationProjections();
        updateDetails();
        updateSummary();
    });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(false);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideRight);
    m_table->setFrameShape(QFrame::NoFrame);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(76);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSortIndicatorShown(true);
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this,
            &MainWindow::onHeaderClicked);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::CheckColumn,
                                                       QHeaderView::Fixed);
    m_table->setColumnWidth(GameTableModel::CheckColumn, 30);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::NameColumn,
                                                       QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::SizeColumn,
                                                       QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::DriveColumn,
                                                       QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::RuntimeColumn,
                                                       QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::LastPlayedColumn,
                                                       QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(GameTableModel::StatusColumn,
                                                       QHeaderView::Interactive);
    m_table->setColumnWidth(GameTableModel::DriveColumn, 150);
    m_table->setColumnWidth(GameTableModel::StatusColumn, 110);
    m_table->setIconSize(QSize(48, 64));
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &MainWindow::onSelectionChanged);
    connect(m_table, &QTableView::doubleClicked, this, &MainWindow::reviewMove);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QWidget::customContextMenuRequested, this,
            &MainWindow::showGameContextMenu);

    m_detailsPanel = new QWidget(right);
    m_detailsPanel->setObjectName(QStringLiteral("detailsPanel"));
    auto *detailsLayout = new QVBoxLayout(m_detailsPanel);
    detailsLayout->setContentsMargins(9, 5, 9, 5);
    detailsLayout->setSpacing(3);
    auto *detailsHeader = new QHBoxLayout();
    detailsHeader->setSpacing(8);
    m_detailsSummary = new QLabel(m_detailsPanel);
    m_detailsSummary->setObjectName(QStringLiteral("detailsSummary"));
    m_detailsSummary->setTextFormat(Qt::PlainText);
    m_detailsSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailsSummary->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    detailsHeader->addWidget(m_detailsSummary, 1);
    m_detailsToggle = new QToolButton(m_detailsPanel);
    m_detailsToggle->setObjectName(QStringLiteral("detailsToggle"));
    m_detailsToggle->setCheckable(true);
    m_detailsToggle->setText(QStringLiteral("Details ▾"));
    m_detailsToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    detailsHeader->addWidget(m_detailsToggle);
    detailsLayout->addLayout(detailsHeader);
    m_details = new QLabel(m_detailsPanel);
    m_details->setObjectName(QStringLiteral("detailsFull"));
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setVisible(false);
    m_details->setMinimumHeight(0);
    detailsLayout->addWidget(m_details);

    auto *contentSplitter = new QSplitter(Qt::Vertical, right);
    contentSplitter->addWidget(m_table);
    contentSplitter->addWidget(m_detailsPanel);
    contentSplitter->setStretchFactor(0, 4);
    contentSplitter->setStretchFactor(1, 1);
    contentSplitter->setChildrenCollapsible(false);
    contentSplitter->setSizes({540, 48});
    rightLayout->addWidget(contentSplitter, 1);
    connect(m_detailsToggle, &QToolButton::toggled, this, [this, contentSplitter](bool open) {
        m_details->setVisible(open);
        m_detailsToggle->setText(open ? QStringLiteral("Details ▴")
                                      : QStringLiteral("Details ▾"));
        contentSplitter->setSizes(open ? QList<int>{420, 260} : QList<int>{540, 48});
    });

    auto *transfer = new QFrame(right);
    transfer->setObjectName(QStringLiteral("transferStrip"));
    auto *transferLayout = new QHBoxLayout(transfer);
    transferLayout->setContentsMargins(12, 8, 12, 8);
    transferLayout->setSpacing(14);
    auto *transferText = new QVBoxLayout();
    transferText->setSpacing(1);
    m_transferEyebrow = new QLabel(QStringLiteral("TRANSFER SUMMARY"), transfer);
    m_transferEyebrow->setObjectName(QStringLiteral("transferEyebrow"));
    m_summary = new QLabel(transfer);
    m_summary->setObjectName(QStringLiteral("transferSummary"));
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sourceSummary = new QLabel(transfer);
    m_sourceSummary->setObjectName(QStringLiteral("transferSource"));
    m_sourceSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    transferText->addWidget(m_transferEyebrow);
    transferText->addWidget(m_summary);
    transferText->addWidget(m_sourceSummary);
    transferLayout->addLayout(transferText, 1);

    auto *destinationBox = new QVBoxLayout();
    destinationBox->setSpacing(2);
    auto *destinationLabel = new QLabel(QStringLiteral("To"), transfer);
    destinationLabel->setObjectName(QStringLiteral("transferEyebrow"));
    destinationBox->addWidget(destinationLabel);
    m_destination = new QComboBox(transfer);
    m_destination->setMinimumWidth(300);
    connect(m_destination, &QComboBox::currentIndexChanged, this, [this](int) {
        updateSummary();
    });
    destinationBox->addWidget(m_destination);
    m_projectionLabel = new QLabel(transfer);
    m_projectionLabel->setObjectName(QStringLiteral("transferProjection"));
    destinationBox->addWidget(m_projectionLabel);
    transferLayout->addLayout(destinationBox);

    m_clearSelection = new QToolButton(transfer);
    m_clearSelection->setObjectName(QStringLiteral("clearSelection"));
    m_clearSelection->setText(QStringLiteral("Clear"));
    m_clearSelection->setToolTip(QStringLiteral("Clear the checked games"));
    connect(m_clearSelection, &QToolButton::clicked, this, [this]() {
        m_model->setCheckedAppIds({});
        m_table->clearSelection();
        updateDestinationProjections();
        updateDetails();
        updateSummary();
    });
    transferLayout->addWidget(m_clearSelection);

    m_reviewButton = new QPushButton(QStringLiteral("Review transfer →"), transfer);
    m_reviewButton->setObjectName(QStringLiteral("reviewButton"));
    m_reviewButton->setDefault(true);
    connect(m_reviewButton, &QPushButton::clicked, this, &MainWindow::reviewMove);
    transferLayout->addWidget(m_reviewButton);
    rightLayout->addWidget(transfer);

    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({310, 950});

    m_artwork = new ArtworkProvider(this);
    connect(m_artwork, &ArtworkProvider::artworkReady, this, &MainWindow::onArtworkReady);

    setCentralWidget(central);
    statusBar()->showMessage(QStringLiteral("Ready"));
    updateSortIndicator();
    applyDensity();
    updateSteamBanner();
}

void MainWindow::populateRoots()
{
    QStringList notes;
    QSettings settings;
    const QString overrideRoot = settings.value(QStringLiteral("steam/root")).toString();
    const QVector<slm::SteamRoot> roots = slm::detectSteamRoots(overrideRoot, &notes);
    if (roots.isEmpty()) {
        if (m_roots.isEmpty())
            setBannerMessage(QStringLiteral("No Steam installation was found. Install Steam, or "
                                            "start the app with a Steam root override."),
                             QStringLiteral("#c0392b"));
        return;
    }
    QString previous;
    if (m_rootCombo->currentIndex() >= 0 && m_rootCombo->currentIndex() < m_roots.size())
        previous = m_roots.at(m_rootCombo->currentIndex()).path;

    m_roots = roots;
    QSignalBlocker blocker(m_rootCombo);
    m_rootCombo->clear();
    for (const slm::SteamRoot &root : m_roots) {
        m_rootCombo->addItem(QStringLiteral("%1 (%2)").arg(root.path, root.kindName()), root.path);
    }
    int index = 0;
    if (!previous.isEmpty()) {
        for (int i = 0; i < m_roots.size(); ++i) {
            if (m_roots.at(i).path == previous) {
                index = i;
                break;
            }
        }
    }
    m_rootCombo->setCurrentIndex(index);
    m_rootCombo->setVisible(m_roots.size() > 1);
}

void MainWindow::rescan()
{
    if (m_moveThread || m_moveProgress) {
        statusBar()->showMessage(QStringLiteral("Cannot rescan while a move queue is running."));
        return;
    }
    if (m_scanThread || m_scanInProgress) {
        statusBar()->showMessage(QStringLiteral("A scan is already running…"));
        return;
    }
    populateRoots();
    if (m_roots.isEmpty()) {
        setBannerMessage(QStringLiteral("No Steam installation was found."), QStringLiteral("#c0392b"));
        return;
    }
    startScan();
}

void MainWindow::startScan()
{
    if (m_roots.isEmpty() || m_scanInProgress)
        return;
    const int index = qBound(0, m_rootCombo->currentIndex(), m_roots.size() - 1);
    const slm::SteamRoot root = m_roots.at(index);
    if (!m_model->hasScan() || m_scan.root.path != root.path) {
        slm::SteamScan cached;
        QString cacheError;
        if (slm::ScanCache::load(root, &cached, &cacheError)) {
            applyCachedScan(cached);
        } else if (m_scan.root.path != root.path) {
            m_scan = slm::SteamScan{};
            m_model->setScan(m_scan);
            populateLibraries();
            populateDestinations();
        }
    }
    m_scanIsCached = true;
    m_scanInProgress = true;
    setCursor(Qt::BusyCursor);
    const bool showingCache = m_model->rowCount() > 0;
    m_headingSubtitle->setText(
        showingCache ? QStringLiteral("Showing cached library · refreshing…")
                     : QStringLiteral("Scanning %1…").arg(root.path));
    setBannerMessage(showingCache
                         ? QStringLiteral("Showing cached library data · refreshing in background…")
                         : QStringLiteral("Scanning %1…").arg(root.path),
                     QStringLiteral("#2d6cdf"));
    m_reviewButton->setEnabled(false);

    auto *thread = new ScanThread(root, this);
    m_scanThread = thread;
    connect(thread, &ScanThread::scanReady, this, &MainWindow::onScanReady);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_scanThread == thread)
            m_scanThread = nullptr;
        m_scanInProgress = false;
        unsetCursor();
        updateSteamBanner();
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::applyCachedScan(const slm::SteamScan &scan)
{
    m_scan = scan;
    m_mounts = slm::MountTable::fromMounts(scan.mounts);
    m_scanIsCached = true;
    m_sizeCache.clear();
    m_sizePending.clear();
    m_model->setScan(scan);
    m_libraryFilter.clear();
    populateLibraries();
    populateDestinations();
    requestArtworkForScan();
    onSelectionChanged();
}

void MainWindow::onScanReady(const slm::SteamScan &scan, const QString &error)
{
    if (!error.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("Scan failed"), error);
        m_headingSubtitle->setText(QStringLiteral("Scan failed"));
        setBannerMessage(QStringLiteral("Scan failed: %1").arg(error), QStringLiteral("#c0392b"));
        return;
    }
    m_scan = scan;
    m_scanIsCached = false;
    m_mounts = slm::MountTable::fromMounts(scan.mounts);
    QString cacheError;
    if (!slm::ScanCache::save(scan, &cacheError)) {
        // Cache is a startup convenience only; failure is not fatal.
    }
    m_sizeCache.clear();
    m_sizePending.clear();
    m_model->setScan(scan);
    m_libraryFilter.clear();
    populateLibraries();
    populateDestinations();
    updateSteamBanner();
    onSelectionChanged();

    int games = 0;
    for (const slm::Game &game : scan.games) {
        if (game.category == slm::GameCategory::Game)
            ++games;
    }
    updateFilterCounts();
    const QString subtitle = QStringLiteral("%1 · %2 libraries · %3 games")
                                 .arg(scan.root.kindName())
                                 .arg(scan.libraries.size())
                                 .arg(games);
    m_headingSubtitle->setText(subtitle);
    statusBar()->showMessage(QStringLiteral("%1 · root %2").arg(subtitle, scan.root.path));
    requestArtworkForScan();

    if (m_firstScan) {
        m_firstScan = false;
        promptUnfinishedOperations();
    }
}

void MainWindow::populateLibraries()
{
    QSignalBlocker blocker(m_libraryList);
    m_libraryList->clear();

    int gameCount = 0;
    for (const slm::Game &game : m_scan.games) {
        if (game.category == slm::GameCategory::Game)
            ++gameCount;
    }
    auto *all = new QListWidgetItem(QStringLiteral("All libraries"));
    all->setData(Qt::UserRole, QString());
    all->setData(ui::SubtitleRole, QStringLiteral("%1 games").arg(gameCount));
    all->setData(ui::UsagePercentRole, -1);
    all->setIcon(ui::allLibrariesIcon());
    all->setToolTip(QStringLiteral("Show games from every library"));
    all->setSizeHint(QSize(0, 70));
    m_libraryList->addItem(all);

    for (const slm::Library &library : m_scan.libraries) {
        int count = 0;
        for (const slm::Game &game : m_scan.games) {
            if (game.category == slm::GameCategory::Game
                && slm::canonicalOrAbsolute(game.libraryPath)
                    == slm::canonicalOrAbsolute(library.path))
                ++count;
        }
        auto *item = new QListWidgetItem(ui::driveDisplayName(library));
        item->setData(Qt::UserRole, library.path);
        item->setData(ui::SubtitleRole, ui::driveSubtitle(library, count));
        const int usage = library.mount.totalBytes > 0
            ? static_cast<int>(((library.mount.totalBytes - library.mount.freeBytes) * 100)
                               / library.mount.totalBytes)
            : -1;
        item->setData(ui::UsagePercentRole, qBound(-1, usage, 100));
        item->setData(ui::TypeColorRole, ui::driveTypeColor(library));
        item->setToolTip(ui::driveTooltip(library));
        item->setSizeHint(QSize(0, 70));
        m_libraryList->addItem(item);
    }
    if (m_libraryList->count() > 0)
        m_libraryList->setCurrentRow(0);
}

void MainWindow::populateDestinations()
{
    const QString previous = m_destination->currentData().toString();
    QSignalBlocker blocker(m_destination);
    m_destination->clear();
    m_destination->addItem(QStringLiteral("Choose destination…"), QString());
    for (const slm::Library &library : m_scan.libraries) {
        const QString label = QStringLiteral("%1 — %2")
                                  .arg(ui::driveDisplayName(library),
                                       library.mount.fsType.isEmpty() ? QStringLiteral("?")
                                                                      : library.mount.fsType.toUpper());
        m_destination->addItem(ui::driveIcon(library), label, library.path);
        m_destination->setItemData(m_destination->count() - 1, ui::driveTooltip(library),
                                   Qt::ToolTipRole);
    }
    if (!previous.isEmpty()) {
        const int index = m_destination->findData(previous);
        if (index >= 0)
            m_destination->setCurrentIndex(index);
    }
    if (m_destination->currentIndex() < 0)
        m_destination->setCurrentIndex(0);
    m_destination->setEnabled(m_destination->count() > 1);
    updateDestinationProjections();
}

void MainWindow::updateDestinationProjections()
{
    const QVector<quint32> selected = selectedAppIds();
    quint64 payload = 0;
    QSet<QString> selectedLibraries;
    for (quint32 appId : selected) {
        const slm::Game *game = m_scan.findGame(appId);
        if (!game)
            continue;
        payload += game->sizeOnDisk;
        selectedLibraries.insert(slm::canonicalOrAbsolute(game->libraryPath));
    }
    auto *model = qobject_cast<QStandardItemModel *>(m_destination->model());
    for (int i = 0; i < m_destination->count(); ++i) {
        const QString path = m_destination->itemData(i).toString();
        if (path.isEmpty()) {
            m_destination->setItemText(i, QStringLiteral("Choose destination…"));
            continue;
        }
        const slm::Library *library = libraryFor(path);
        if (!library)
            continue;
        QString label = ui::driveDisplayName(*library);
        bool blocked = false;
        QString reason;
        if (selectedLibraries.contains(slm::canonicalOrAbsolute(library->path))) {
            blocked = true;
            label += QStringLiteral(" · current library");
            reason = QStringLiteral("This is the source library of at least one selected game.");
        } else if (payload > 0 && payload > library->mount.freeBytes) {
            blocked = true;
            label += QStringLiteral(" — needs %1, only %2 free")
                         .arg(slm::formatBytes(payload),
                              slm::formatBytes(library->mount.freeBytes));
            reason = QStringLiteral("Not enough free space for the selected game payload.");
        } else {
            label += QStringLiteral(" — %1 free").arg(slm::formatBytes(library->mount.freeBytes));
        }
        m_destination->setItemText(i, label);
        QString tooltip = ui::driveTooltip(*library);
        if (!reason.isEmpty())
            tooltip += QStringLiteral("\n\n%1").arg(reason);
        if (payload > 0) {
            tooltip += QStringLiteral("\nEstimated payload after move: %1 (prefix/shadercache may "
                                      "add more).")
                           .arg(slm::formatBytes(payload));
        }
        m_destination->setItemData(i, tooltip, Qt::ToolTipRole);
        if (model) {
            if (QStandardItem *item = model->item(i)) {
                Qt::ItemFlags flags = item->flags() | Qt::ItemIsEnabled;
                if (blocked)
                    flags &= ~Qt::ItemIsEnabled;
                item->setFlags(flags);
            }
        }
    }
    if (m_destination->currentIndex() > 0) {
        const QModelIndex current = m_destination->model()->index(m_destination->currentIndex(), 0);
        if (!(current.flags() & Qt::ItemIsEnabled)) {
            const QSignalBlocker blocker(m_destination);
            m_destination->setCurrentIndex(0);
        }
    }
}

void MainWindow::onRootChanged(int index)
{
    Q_UNUSED(index);
    if (m_moveThread || m_moveProgress)
        return;
    if (m_scanInProgress || m_roots.isEmpty())
        return;
    startScan();
}

void MainWindow::onLibrarySelected()
{
    const QListWidgetItem *item = m_libraryList->currentItem();
    if (!item)
        return;
    m_libraryFilter = item->data(Qt::UserRole).toString();
    m_model->setLibraryFilter(m_libraryFilter);
    onSelectionChanged();
}

void MainWindow::onSelectionChanged()
{
    if (m_model && m_table && m_table->selectionModel()) {
        QSet<quint32> checked;
        for (quint32 appId : m_model->checkedAppIds())
            checked.insert(appId);
        for (const QModelIndex &index : m_table->selectionModel()->selectedRows()) {
            bool ok = false;
            const quint32 appId = index.data(GameTableModel::AppIdRole).toUInt(&ok);
            if (ok && appId != 0)
                checked.insert(appId);
        }
        m_model->setCheckedAppIds(checked);
    }
    updateDestinationProjections();
    updateDetails();
    updateSummary();
}

void MainWindow::onSearchChanged(const QString &text)
{
    m_model->setSearchText(text);
    onSelectionChanged();
}

void MainWindow::onShowToolsToggled(bool show)
{
    m_model->setShowTools(show);
    updateFilterCounts();
    onSelectionChanged();
}

void MainWindow::showGameContextMenu(const QPoint &pos)
{
    const QModelIndex clicked = m_table->indexAt(pos);
    if (!clicked.isValid())
        return;
    bool ok = false;
    const quint32 clickedAppId = clicked.data(GameTableModel::AppIdRole).toUInt(&ok);
    if (!ok || clickedAppId == 0)
        return;
    if (!m_table->selectionModel()->isRowSelected(clicked.row(), QModelIndex())) {
        m_table->clearSelection();
        m_table->selectRow(clicked.row());
    }
    const slm::Game *game = m_scan.findGame(clickedAppId);
    if (!game)
        return;
    const slm::Library *library = libraryFor(game->libraryPath);
    if (!library)
        return;

    QMenu menu(this);
    const QVector<quint32> selected = selectedAppIds();
    const QString moveLabel = selected.size() > 1
        ? QStringLiteral("Move %1 selected games to").arg(selected.size())
        : QStringLiteral("Move to");
    QMenu *moveMenu = menu.addMenu(moveLabel);
    bool anyDestination = false;
    for (int i = 0; i < m_destination->count(); ++i) {
        const QString path = m_destination->itemData(i).toString();
        if (slm::canonicalOrAbsolute(path) == slm::canonicalOrAbsolute(library->path))
            continue;
        anyDestination = true;
        QAction *action = moveMenu->addAction(m_destination->itemText(i));
        const QString destination = path;
        connect(action, &QAction::triggered, this, [this, destination, i]() {
            m_destination->setCurrentIndex(i);
            reviewMove();
        });
    }
    moveMenu->setEnabled(anyDestination);
    menu.addSeparator();

    const QString payload = library->steamappsPath()
        + QStringLiteral("/common/") + game->installDir;
    QAction *openGame = menu.addAction(QStringLiteral("Open install folder"));
    connect(openGame, &QAction::triggered, this,
            [payload]() { QDesktopServices::openUrl(QUrl::fromLocalFile(payload)); });

    bool prefixExists = false;
    const QString prefix = slm::resolvePrefixPath(*library, game->appId, &prefixExists);
    QAction *openPrefix = menu.addAction(QStringLiteral("Open Proton prefix"));
    openPrefix->setEnabled(prefixExists);
    connect(openPrefix, &QAction::triggered, this,
            [prefix]() { QDesktopServices::openUrl(QUrl::fromLocalFile(prefix)); });

    QAction *openManifest = menu.addAction(QStringLiteral("Open manifest folder"));
    connect(openManifest, &QAction::triggered, this, [game]() {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(game->manifestPath).absolutePath()));
    });
    menu.addSeparator();
    QAction *copyAppId = menu.addAction(QStringLiteral("Copy appid %1").arg(game->appId));
    connect(copyAppId, &QAction::triggered, this, [game]() {
        QApplication::clipboard()->setText(QString::number(game->appId));
    });
    QAction *steamDb = menu.addAction(QStringLiteral("Open on SteamDB"));
    connect(steamDb, &QAction::triggered, this, [game]() {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://steamdb.info/app/%1/").arg(game->appId)));
    });
    if (game->category == slm::GameCategory::Game
        && game->runtime != slm::RuntimeKind::Native) {
        QAction *protonDb = menu.addAction(QStringLiteral("Open on ProtonDB"));
        connect(protonDb, &QAction::triggered, this, [game]() {
            QDesktopServices::openUrl(
                QUrl(QStringLiteral("https://www.protondb.com/app/%1").arg(game->appId)));
        });
    }
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void MainWindow::applyDensity()
{
    const bool compact =
        QSettings().value(QStringLiteral("appearance/compact"), true).toBool();
    m_table->verticalHeader()->setDefaultSectionSize(compact ? 54 : 76);
    m_table->setIconSize(compact ? QSize(34, 46) : QSize(48, 64));
    m_headingSubtitle->setVisible(!compact);
    QFont headingFont = QApplication::font();
    headingFont.setPointSizeF(headingFont.pointSizeF() + (compact ? 4.0 : 8.0));
    headingFont.setBold(true);
    m_heading->setFont(headingFont);
    if (m_detailsPanel)
        m_detailsPanel->setMinimumHeight(compact ? 36 : 42);
}

void MainWindow::onHeaderClicked(int section)
{
    GameTableModel::SortMode mode = GameTableModel::SortMode::Name;
    switch (section) {
    case GameTableModel::CheckColumn:
        return;
    case GameTableModel::NameColumn:
        mode = GameTableModel::SortMode::Name;
        break;
    case GameTableModel::SizeColumn:
        mode = GameTableModel::SortMode::SizeLargest;
        break;
    case GameTableModel::DriveColumn:
        mode = GameTableModel::SortMode::Drive;
        break;
    case GameTableModel::RuntimeColumn:
        mode = GameTableModel::SortMode::Runtime;
        break;
    case GameTableModel::LastPlayedColumn:
        mode = GameTableModel::SortMode::LastPlayed;
        break;
    case GameTableModel::StatusColumn:
        mode = GameTableModel::SortMode::Status;
        break;
    default:
        return;
    }
    bool ascending = true;
    if (m_model->sortMode() == mode)
        ascending = !m_model->sortAscending();
    else if (mode == GameTableModel::SortMode::SizeLargest
             || mode == GameTableModel::SortMode::LastPlayed)
        ascending = false;
    m_model->setSort(mode, ascending);
    const QSignalBlocker blocker(m_sortMode);
    const int comboIndex = m_sortMode->findData(static_cast<int>(mode));
    if (comboIndex >= 0)
        m_sortMode->setCurrentIndex(comboIndex);
    m_table->horizontalHeader()->setSortIndicator(
        section, ascending ? Qt::AscendingOrder : Qt::DescendingOrder);
    onSelectionChanged();
}

void MainWindow::updateSortIndicator()
{
    int section = GameTableModel::NameColumn;
    switch (m_model->sortMode()) {
    case GameTableModel::SortMode::Name:
        section = GameTableModel::NameColumn;
        break;
    case GameTableModel::SortMode::SizeLargest:
        section = GameTableModel::SizeColumn;
        break;
    case GameTableModel::SortMode::Drive:
        section = GameTableModel::DriveColumn;
        break;
    case GameTableModel::SortMode::Runtime:
        section = GameTableModel::RuntimeColumn;
        break;
    case GameTableModel::SortMode::LastPlayed:
        section = GameTableModel::LastPlayedColumn;
        break;
    case GameTableModel::SortMode::Status:
        section = GameTableModel::StatusColumn;
        break;
    }
    m_table->horizontalHeader()->setSortIndicator(
        section, m_model->sortAscending() ? Qt::AscendingOrder : Qt::DescendingOrder);
}

void MainWindow::updateFilterCounts()
{
    int ready = 0;
    int blocked = 0;
    int warnings = 0;
    for (const slm::Game &game : m_scan.games) {
        if (!m_showToolsAction->isChecked()
            && (game.category == slm::GameCategory::Tool
                || game.category == slm::GameCategory::SystemComponent
                || game.category == slm::GameCategory::Broken))
            continue;
        bool blocker = false;
        bool warning = false;
        for (const slm::Issue &issue : game.issues) {
            blocker = blocker || issue.isBlocker();
            warning = warning || !issue.isBlocker();
        }
        if (blocker)
            ++blocked;
        else if (warning)
            ++warnings;
        else
            ++ready;
    }
    m_statusFilter->setItemText(0, QStringLiteral("All statuses (%1)").arg(ready + blocked + warnings));
    m_statusFilter->setItemText(1, QStringLiteral("Ready (%1)").arg(ready));
    m_statusFilter->setItemText(2, QStringLiteral("Blocked (%1)").arg(blocked));
    m_statusFilter->setItemText(3, QStringLiteral("Warnings (%1)").arg(warnings));
}

void MainWindow::onGamesDroppedOnLibrary(const QString &libraryPath,
                                         const QList<quint32> &appIds)
{
    if (libraryPath.isEmpty() || appIds.isEmpty())
        return;
    selectGamesByIds(appIds);
    const int index = m_destination->findData(libraryPath);
    if (index >= 0)
        m_destination->setCurrentIndex(index);
    updateSummary();
    reviewMove();
}

void MainWindow::selectGamesByIds(const QList<quint32> &appIds)
{
    if (!m_table->selectionModel() || !m_model)
        return;
    QSet<quint32> wanted;
    for (quint32 appId : appIds)
        wanted.insert(appId);
    m_model->setCheckedAppIds({});
    m_table->clearSelection();
    QModelIndex first;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex nameIndex = m_model->index(row, GameTableModel::NameColumn);
        bool ok = false;
        const quint32 appId = nameIndex.data(GameTableModel::AppIdRole).toUInt(&ok);
        if (!ok || !wanted.contains(appId))
            continue;
        m_table->selectionModel()->select(
            nameIndex, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        if (!first.isValid())
            first = nameIndex;
    }
    if (first.isValid()) {
        m_table->setCurrentIndex(first);
        m_table->scrollTo(first);
    } else {
        QMessageBox::information(this, QStringLiteral("Games not visible"),
                                 QStringLiteral("The dropped games are hidden by the current filter."));
    }
}

QVector<quint32> MainWindow::selectedAppIds() const
{
    return m_model ? m_model->checkedAppIds() : QVector<quint32>();
}

const slm::Game *MainWindow::selectedGame() const
{
    const QModelIndex index = m_table->currentIndex();
    if (!index.isValid())
        return nullptr;
    return &m_model->gameAt(index.row());
}

const slm::Library *MainWindow::libraryFor(const QString &path) const
{
    const QString canonical = slm::canonicalOrAbsolute(path);
    for (const slm::Library &library : m_scan.libraries) {
        if (slm::canonicalOrAbsolute(library.path) == canonical)
            return &library;
    }
    return nullptr;
}

QString MainWindow::selectedDestination() const
{
    return m_destination->currentData().toString();
}

void MainWindow::updateDetails()
{
    const QString muted = ui::isDarkTheme() ? QStringLiteral("#aab4c0")
                                            : QStringLiteral("#66717d");
    const slm::Game *game = selectedGame();
    if (!game) {
        QString summary;
        QString html;
        if (!m_libraryFilter.isEmpty()) {
            const slm::Library *library = libraryFor(m_libraryFilter);
            if (library) {
                int count = 0;
                quint64 total = 0;
                const slm::Game *largest = nullptr;
                for (const slm::Game &candidate : m_scan.games) {
                    if (candidate.category != slm::GameCategory::Game)
                        continue;
                    if (slm::canonicalOrAbsolute(candidate.libraryPath)
                        != slm::canonicalOrAbsolute(library->path))
                        continue;
                    ++count;
                    total += candidate.sizeOnDisk;
                    if (!largest || candidate.sizeOnDisk > largest->sizeOnDisk)
                        largest = &candidate;
                }
                html = QStringLiteral("<b>%1</b> <span style='color:%2'>· %3 · %4 free · "
                                      "%5 game%6</span><br>")
                           .arg(ui::driveDisplayName(*library).toHtmlEscaped(), muted,
                                library->mount.fsType.toHtmlEscaped(),
                                slm::formatBytes(library->mount.freeBytes))
                           .arg(count)
                           .arg(count == 1 ? QString() : QStringLiteral("s"));
                html += QStringLiteral("Installed: <b>%1</b>")
                            .arg(slm::formatBytes(total));
                if (largest) {
                    html += QStringLiteral(" &nbsp; Largest: <b>%1</b> (%2)")
                                .arg(largest->name.toHtmlEscaped(),
                                     slm::formatBytes(largest->sizeOnDisk));
                }
                html += QStringLiteral("<br>%1<br><br>")
                            .arg(compatDataDescription(*library).toHtmlEscaped());
                summary = QStringLiteral("%1 · %2 games · %3 free")
                              .arg(ui::driveDisplayName(*library))
                              .arg(count)
                              .arg(slm::formatBytes(library->mount.freeBytes));
            }
        }
        if (html.isEmpty()) {
            int count = 0;
            quint64 total = 0;
            for (const slm::Game &candidate : m_scan.games) {
                if (candidate.category != slm::GameCategory::Game)
                    continue;
                ++count;
                total += candidate.sizeOnDisk;
            }
            html = QStringLiteral("<b>All libraries</b><br>");
            html += QStringLiteral("%1 games across %2 libraries · %3 installed<br>")
                        .arg(count)
                        .arg(m_scan.libraries.size())
                        .arg(slm::formatBytes(total));
            if (!m_scan.root.path.isEmpty())
                html += QStringLiteral("%1 · %2<br>")
                            .arg(m_scan.root.kindName(), m_scan.root.path.toHtmlEscaped());
            summary = QStringLiteral("All libraries · %1 games · %2 installed")
                          .arg(count)
                          .arg(slm::formatBytes(total));
        }
        html += QStringLiteral("<br><span style='color:%1'>Select a game for details, or drag "
                               "games from the list onto a library to move them.</span>")
                    .arg(muted);
        if (m_detailsSummary)
            m_detailsSummary->setText(summary.isEmpty() ? QStringLiteral("Select a game")
                                                         : summary);
        m_details->setText(html);
        return;
    }
    const slm::Library *library = libraryFor(game->libraryPath);
    QString html = QStringLiteral("<b>%1</b> &nbsp; <span style='color:%2'>appid %3</span><br>")
                       .arg(game->name.toHtmlEscaped(), muted)
                       .arg(game->appId);
    if (library) {
        html += QStringLiteral("Drive: <b>%1</b> <span style='color:%3'>%2</span><br>")
                    .arg(ui::driveDisplayName(*library).toHtmlEscaped(),
                         library->rawPath.toHtmlEscaped(), muted);
        html += QStringLiteral("Filesystem: %1 (%2) &nbsp; free: %3<br>")
                    .arg(library->mount.fsType,
                         library->mount.readOnly ? QStringLiteral("read-only")
                                                 : QStringLiteral("read-write"),
                         slm::formatBytes(library->mount.freeBytes));
        html += QStringLiteral("compatdata: %1<br>").arg(compatDataDescription(*library).toHtmlEscaped());
    } else {
        html += QStringLiteral("Library: %1<br>").arg(game->libraryPath.toHtmlEscaped());
    }
    const QString runtime = slm::runtimeKindName(game->runtime);
    html += QStringLiteral("Runtime: <b>%1</b>").arg(runtime);
    if (!game->protonVersion.isEmpty())
        html += QStringLiteral(" <span style='color:%1'>(%2)</span>")
                    .arg(muted, game->protonVersion.toHtmlEscaped());
    html += QStringLiteral("<br>");
    const QString lastPlayed = game->lastPlayed > 0
        ? QLocale().toString(QDateTime::fromSecsSinceEpoch(game->lastPlayed),
                             QLocale::ShortFormat)
        : QStringLiteral("Never");
    html += QStringLiteral("Last played: %1 &nbsp; Build ID: %2<br>")
                .arg(lastPlayed,
                     game->buildId == 0 ? QStringLiteral("—") : QString::number(game->buildId));
    html += QStringLiteral("Install directory: %1<br>").arg(game->installDir.toHtmlEscaped());
    html += QStringLiteral("Manifest: %1<br>").arg(game->manifestPath.toHtmlEscaped());

    const auto cachedSizes = m_sizeCache.constFind(game->appId);
    if (cachedSizes != m_sizeCache.constEnd() && cachedSizes->ready) {
        html += QStringLiteral("Payload: %1 &nbsp; prefix %2 &nbsp; shader %3 &nbsp; workshop %4<br>")
                    .arg(game->sizeOnDisk == 0 ? QStringLiteral("unknown")
                                               : slm::formatBytes(game->sizeOnDisk),
                         slm::formatBytes(cachedSizes->prefix), slm::formatBytes(cachedSizes->shader),
                         slm::formatBytes(cachedSizes->workshop));
    } else {
        html += QStringLiteral("Payload: %1 &nbsp; <span style='color:%2'>prefix/shadercache/"
                               "workshop: calculating…</span><br>")
                    .arg(game->sizeOnDisk == 0 ? QStringLiteral("unknown")
                                               : slm::formatBytes(game->sizeOnDisk), muted);
        if (library && !m_sizePending.contains(game->appId)
            && !m_sizeCache.contains(game->appId)) {
            m_sizePending.insert(game->appId);
            auto *thread = new SizeThread(*library, *game, this);
            m_sizeThreads.append(thread);
            connect(thread, &SizeThread::sizesReady, this, &MainWindow::onSizesReady);
            connect(thread, &QThread::finished, this, [this, thread]() {
                m_sizeThreads.removeAll(QPointer<SizeThread>(thread));
                thread->deleteLater();
            });
            thread->start();
        }
    }
    if (!game->issues.isEmpty()) {
        html += QStringLiteral("<br>");
        for (const slm::Issue &issue : game->issues) {
            const QString issueColor = issue.isBlocker()
                ? (ui::isDarkTheme() ? QStringLiteral("#ff6b6b") : QStringLiteral("#b72b2b"))
                : (ui::isDarkTheme() ? QStringLiteral("#ffb84d") : QStringLiteral("#9a6500"));
            html += QStringLiteral("<span style='color:%1'>%2:</span> %3")
                        .arg(issueColor,
                             issue.isBlocker() ? QStringLiteral("Blocker") : QStringLiteral("Warning"),
                             issue.message.toHtmlEscaped());
            if (!issue.detail.isEmpty())
                html += QStringLiteral(" — <span style='color:%1'>%2</span>")
                            .arg(muted, issue.detail.toHtmlEscaped());
            html += QStringLiteral("<br>");
        }
    }
    if (m_detailsSummary) {
        QStringList summary;
        summary << game->name;
        summary << slm::runtimeKindName(game->runtime);
        summary << (game->sizeOnDisk == 0 ? QStringLiteral("unknown size")
                                          : slm::formatBytes(game->sizeOnDisk));
        if (library)
            summary << ui::driveDisplayName(*library);
        const auto issueCount = game->issues.size();
        if (issueCount > 0)
            summary << QStringLiteral("%1 finding%2").arg(issueCount).arg(issueCount == 1 ? QString() : QStringLiteral("s"));
        m_detailsSummary->setText(summary.join(QStringLiteral(" · ")));
    }
    m_details->setText(html);
}

quint64 MainWindow::selectedPayloadBytes() const
{
    quint64 total = 0;
    for (quint32 appId : selectedAppIds()) {
        if (const slm::Game *game = m_scan.findGame(appId))
            total += game->sizeOnDisk;
    }
    return total;
}

QString MainWindow::reviewBlockReason() const
{
    if (m_scanInProgress || !m_scanThread.isNull())
        return QStringLiteral("Scanning the Steam libraries…");
    if (m_scanIsCached)
        return QStringLiteral("Showing cached library data; wait for the refresh to finish.");
    if (m_moveThread || (m_moveProgress && !m_queueFinished))
        return QStringLiteral("A move queue is already running.");
    const QVector<quint32> appIds = selectedAppIds();
    if (appIds.isEmpty())
        return QStringLiteral("Select one or more games first.");
    const QString destination = selectedDestination();
    if (destination.isEmpty())
        return QStringLiteral("Choose a destination library first.");
    for (quint32 appId : appIds) {
        const slm::Game *game = m_scan.findGame(appId);
        if (!game)
            continue;
        if (slm::canonicalOrAbsolute(game->libraryPath)
            == slm::canonicalOrAbsolute(destination)) {
            return QStringLiteral("The destination is the current library of %1.")
                .arg(game->name);
        }
    }
    if (const slm::Library *library = libraryFor(destination)) {
        const quint64 payload = selectedPayloadBytes();
        if (payload > library->mount.freeBytes) {
            return QStringLiteral("Not enough free space: %1 needed, %2 available.")
                .arg(slm::formatBytes(payload), slm::formatBytes(library->mount.freeBytes));
        }
    }
    if (m_steamRunning)
        return QStringLiteral("Steam is running. Quit Steam before moving files.");
    return {};
}

void MainWindow::updateSummary()
{
    const QVector<quint32> appIds = selectedAppIds();
    const quint64 payload = selectedPayloadBytes();
    if (appIds.isEmpty()) {
        m_summary->setText(QStringLiteral("Select games to transfer"));
        if (m_sourceSummary)
            m_sourceSummary->setText(QStringLiteral("Use the checkboxes in the game list."));
    } else {
        m_summary->setText(
            QStringLiteral("%1 game%2 selected · %3 payload")
                .arg(appIds.size())
                .arg(appIds.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(payload == 0 ? QStringLiteral("unknown size")
                                  : slm::formatBytes(payload)));
    }
    if (m_selectionCount) {
        m_selectionCount->setText(
            appIds.isEmpty()
                ? QStringLiteral("%1 games").arg(m_model ? m_model->rowCount() : 0)
                : QStringLiteral("%1 games · %2 selected")
                      .arg(m_model ? m_model->rowCount() : 0)
                      .arg(appIds.size()));
    }

    QStringList sources;
    for (quint32 appId : appIds) {
        if (const slm::Game *game = m_scan.findGame(appId)) {
            const slm::Library *source = libraryFor(game->libraryPath);
            const QString name = source ? ui::driveDisplayName(*source) : game->libraryPath;
            if (!sources.contains(name))
                sources.append(name);
        }
    }
    if (m_sourceSummary) {
        if (appIds.isEmpty())
            m_sourceSummary->setText(QStringLiteral("No games checked."));
        else
            m_sourceSummary->setText(
                QStringLiteral("From %1").arg(sources.join(QStringLiteral(" + "))));
    }
    const slm::Library *destinationLibrary = libraryFor(selectedDestination());
    if (m_projectionLabel) {
        if (!destinationLibrary) {
            m_projectionLabel->setText(QStringLiteral("Choose a destination library"));
        } else {
            const quint64 remaining = destinationLibrary->mount.freeBytes > payload
                ? destinationLibrary->mount.freeBytes - payload : 0;
            m_projectionLabel->setText(
                payload > 0
                    ? QStringLiteral("~%1 free after · %2 payload")
                          .arg(slm::formatBytes(remaining), slm::formatBytes(payload))
                    : QStringLiteral("%1 free")
                          .arg(slm::formatBytes(destinationLibrary->mount.freeBytes)));
        }
    }
    if (m_clearSelection)
        m_clearSelection->setEnabled(!appIds.isEmpty());
    QStringList details;
    details << QStringLiteral("%1 selected").arg(appIds.size());
    details << QStringLiteral("Payload: %1")
                   .arg(selectedPayloadBytes() == 0
                            ? QStringLiteral("unknown")
                            : slm::formatBytes(selectedPayloadBytes()));
    if (!sources.isEmpty())
        details << QStringLiteral("Source libraries: %1").arg(sources.join(QStringLiteral(", ")));
    if (destinationLibrary) {
        details << QStringLiteral("Destination: %1 (%2 free)")
                       .arg(ui::driveDisplayName(*destinationLibrary),
                            slm::formatBytes(destinationLibrary->mount.freeBytes));
        if (const quint64 payload = selectedPayloadBytes(); payload > 0) {
            const quint64 remaining = destinationLibrary->mount.freeBytes > payload
                ? destinationLibrary->mount.freeBytes - payload : 0;
            details << QStringLiteral("Destination after move: ~%1 free")
                           .arg(slm::formatBytes(remaining));
            details << QStringLiteral("Space recovered on source after cleanup: %1")
                           .arg(slm::formatBytes(payload));
        }
        details << QStringLiteral("Copy/verify temporarily needs destination space for the "
                                  "payload plus prefix/shadercache when those are moved.");
    }
    const QString blockReason = reviewBlockReason();
    m_reviewButton->setEnabled(blockReason.isEmpty());
    m_reviewButton->setToolTip(blockReason.isEmpty()
                                   ? QStringLiteral("Review the selected move")
                                   : blockReason);
    if (!blockReason.isEmpty())
        details.prepend(blockReason);
    m_summary->setToolTip(details.join(QLatin1Char('\n')));
}

void MainWindow::updateSteamBanner()
{
    if (m_scanInProgress || !m_scanThread.isNull())
        return;
    const slm::ProcessUsage usage = slm::inspectProcesses({}, {});
    m_steamRunning = usage.steamRunning;
    if (usage.steamRunning) {
        setBannerMessage(
            QStringLiteral("Steam is running. It must be completely closed before moving files. "
                           "Use “Quit Steam” and wait for it to exit."),
            QStringLiteral("#c0392b"));
        m_quitSteamAction->setEnabled(true);
    } else if (m_scan.root.path.isEmpty()) {
        setBannerMessage(QStringLiteral("No Steam installation loaded yet."),
                         QStringLiteral("#7f8c8d"));
        m_quitSteamAction->setEnabled(false);
    } else {
        setBannerMessage(
            QStringLiteral("✓ Steam is closed"),
            QStringLiteral("#2e8b57"), true);
        m_banner->setToolTip(
            QStringLiteral("Steam is closed. Moves copy and verify the game, then switch the "
                           "manifest atomically. Close all Proton/Wine games before starting."));
        m_quitSteamAction->setEnabled(false);
    }
    updateSummary();
}

void MainWindow::setBannerMessage(const QString &text, const QString &accent, bool compact)
{
    m_banner->setText(text);
    m_banner->setContentsMargins(compact ? 8 : 12, compact ? 1 : 8, compact ? 8 : 12,
                                 compact ? 1 : 8);
    if (compact) {
        const int compactHeight = qMax(26, m_banner->fontMetrics().height() + 8);
        m_banner->setMinimumHeight(compactHeight);
        m_banner->setMaximumHeight(compactHeight);
    } else {
        m_banner->setMinimumHeight(0);
        m_banner->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    const QPalette palette = this->palette();
    const QString padding = compact ? QStringLiteral("2px 8px") : QStringLiteral("8px 12px");
    m_banner->setStyleSheet(
        QStringLiteral("QLabel { background-color: %1; color: %2; border: 1px solid %3; "
                       "border-left: 4px solid %4; border-radius: 6px; padding: %5; }")
            .arg(palette.color(QPalette::Base).name(), palette.color(QPalette::Text).name(),
                 palette.color(QPalette::Mid).name(), accent, padding));
    m_banner->setToolTip(QString());
}

void MainWindow::refreshTheme()
{
    const QPalette palette = QApplication::palette();
    for (QWidget *widget : QApplication::allWidgets()) {
        widget->setPalette(palette);
        widget->update();
    }
    if (QWidget *central = centralWidget()) {
        const QString sheet = central->styleSheet();
        central->setStyleSheet(QString());
        central->setStyleSheet(sheet);
    }
    if (m_selectionCount)
        setMutedLabel(m_selectionCount);
    if (m_headingSubtitle) {
        QPalette subtitlePalette = m_headingSubtitle->palette();
        subtitlePalette.setColor(QPalette::WindowText,
                                 ui::isDarkTheme() ? QColor(QStringLiteral("#9aa4af"))
                                                   : QColor(QStringLiteral("#6b7480")));
        m_headingSubtitle->setPalette(subtitlePalette);
    }
    if (m_rescanAction)
        m_rescanAction->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    if (m_quitSteamAction)
        m_quitSteamAction->setIcon(QIcon::fromTheme(QStringLiteral("application-exit")));
    for (int i = 0; i < m_libraryList->count(); ++i) {
        QListWidgetItem *item = m_libraryList->item(i);
        if (!item)
            continue;
        const QString path = item->data(Qt::UserRole).toString();
        if (path.isEmpty()) {
            item->setIcon(ui::allLibrariesIcon());
        } else if (const slm::Library *library = libraryFor(path)) {
            item->setIcon(QIcon());
            item->setData(ui::TypeColorRole, ui::driveTypeColor(*library));
        }
    }
    setWindowIcon(QIcon::fromTheme(QStringLiteral("steam-library-mover")));
    updateSteamBanner();
    updateDetails();
    update();
}

void MainWindow::requestArtworkForScan()
{
    if (!m_artwork)
        return;
    QSettings settings;
    m_artwork->setEnabled(settings.value(QStringLiteral("artwork/download"), true).toBool());
    if (!m_artwork->isEnabled())
        return;
    for (const slm::Game &game : m_scan.games) {
        if (game.appId == 0)
            continue;
        m_artwork->request(game.appId, game.artworkPath);
    }
}

void MainWindow::onArtworkReady(quint32 appId, const QString &path)
{
    m_model->setArtwork(appId, path);
}

void MainWindow::onSizesReady(quint32 appId, quint64 prefixBytes, quint64 shaderBytes,
                              quint64 workshopBytes)
{
    GameSizes sizes;
    sizes.prefix = prefixBytes;
    sizes.shader = shaderBytes;
    sizes.workshop = workshopBytes;
    sizes.ready = true;
    m_sizeCache.insert(appId, sizes);
    m_sizePending.remove(appId);
    const slm::Game *game = selectedGame();
    if (game && game->appId == appId)
        updateDetails();
}

slm::MoveOptions MainWindow::optionsFromSettings() const
{
    QSettings settings;
    slm::MoveOptions options;
    options.moveShaderCache = settings.value(QStringLiteral("components/shadercache"), true).toBool();
    options.moveWorkshop = settings.value(QStringLiteral("components/workshop"), true).toBool();
    options.fullVerify = settings.value(QStringLiteral("verify/full"), false).toBool();
    options.allowNtfsNative = true; // the GUI requires an explicit acknowledgement in review
    return options;
}

void MainWindow::reviewMove()
{
    if (m_scanThread || m_planThread || m_moveThread || m_planProgress || m_moveProgress)
        return;
    if (m_scanIsCached) {
        QMessageBox::information(this, QStringLiteral("Refreshing library"),
                                 QStringLiteral("The library view is from a cached scan. Wait "
                                                "for the background refresh to finish before moving "
                                                "games."));
        return;
    }
    const QVector<quint32> appIds = selectedAppIds();
    if (appIds.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No games selected"),
                                 QStringLiteral("Select one or more games first."));
        return;
    }
    const QString destination = selectedDestination();
    if (destination.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("No destination"),
                                 QStringLiteral("No destination Steam library is available."));
        return;
    }
    const slm::ProcessUsage usage = slm::inspectProcesses({}, {});
    if (usage.steamRunning) {
        QMessageBox::warning(this, QStringLiteral("Steam is running"),
                             QStringLiteral("Quit Steam completely before moving games."));
        updateSteamBanner();
        return;
    }

    m_planAppIds = appIds;
    m_batchDestination = destination;
    m_batchReserved.clear();
    m_batchPlans.clear();
    m_planIndex = 0;
    m_batchOptions = optionsFromSettings();
    m_batchOptions.allowNtfsNative = true;
    m_batchStopReason.clear();
    m_batchResults.clear();

    auto *progress = new QProgressDialog(
        QStringLiteral("Planning game 1 of %1…").arg(appIds.size()), QString(), 0, appIds.size(),
        this);
    progress->setWindowTitle(QStringLiteral("Steam Library Mover"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setCancelButton(nullptr);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->show();
    m_planProgress = progress;
    planNextGame();
}

void MainWindow::planNextGame()
{
    if (m_planIndex >= m_planAppIds.size()) {
        if (m_planProgress) {
            m_planProgress->close();
            m_planProgress->deleteLater();
            m_planProgress = nullptr;
        }
        showBatchReview();
        return;
    }

    const quint32 appId = m_planAppIds.at(m_planIndex);
    const slm::Game *game = m_scan.findGame(appId);
    if (!game) {
        if (m_planProgress) {
            m_planProgress->close();
            m_planProgress->deleteLater();
            m_planProgress = nullptr;
        }
        QMessageBox::critical(this, QStringLiteral("Planning failed"),
                              QStringLiteral("App %1 is no longer in the scan.").arg(appId));
        return;
    }
    if (m_planProgress) {
        m_planProgress->setLabelText(
            QStringLiteral("Planning game %1 of %2: %3")
                .arg(m_planIndex + 1)
                .arg(m_planAppIds.size())
                .arg(game->name));
        m_planProgress->setValue(m_planIndex);
    }

    auto *thread = new PlanThread(m_scan, appId, m_batchDestination, m_batchOptions,
                                  m_batchReserved, this);
    m_planThread = thread;
    connect(thread, &PlanThread::planReady, this, &MainWindow::onPlanReady);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_planThread == thread)
            m_planThread = nullptr;
        thread->deleteLater();
    });
    thread->start();
}

void MainWindow::onPlanReady(const slm::MovePlan &plan, const QString &error)
{
    if (!error.isEmpty()) {
        if (m_planProgress) {
            m_planProgress->close();
            m_planProgress->deleteLater();
            m_planProgress = nullptr;
        }
        m_batchPlans.clear();
        m_planAppIds.clear();
        QMessageBox::critical(this, QStringLiteral("Cannot plan move"), error);
        return;
    }

    m_batchPlans.append(plan);
    if (!plan.hasBlocker()) {
        for (auto it = plan.requiredPerMount.constBegin();
             it != plan.requiredPerMount.constEnd(); ++it)
            m_batchReserved[it.key()] += it.value();
    }
    ++m_planIndex;
    if (m_planProgress) {
        m_planProgress->setValue(m_planIndex);
        m_planProgress->setLabelText(
            QStringLiteral("Planning %1 of %2…")
                .arg(qMin(m_planIndex + 1, m_planAppIds.size()))
                .arg(m_planAppIds.size()));
    }
    planNextGame();
}

void MainWindow::showBatchReview()
{
    if (m_batchPlans.isEmpty())
        return;

    bool anyBlocker = false;
    bool ntfsWarning = false;
    quint64 totalBytes = 0;
    quint64 payloadBytes = 0;
    QStringList sources;
    QStringList issueText;
    QStringList seen;
    QVector<slm::Issue> blockers;
    QSet<QString> blockerSeen;
    for (const slm::MovePlan &plan : m_batchPlans) {
        ntfsWarning = ntfsWarning || hasNtfsWarning(plan);
        totalBytes += plan.totalBytes;
        payloadBytes += plan.game.sizeOnDisk;
        const slm::Library *sourceLibrary = libraryFor(plan.game.libraryPath);
        const QString sourceName = sourceLibrary ? ui::driveDisplayName(*sourceLibrary)
                                                 : plan.game.libraryPath;
        if (!sources.contains(sourceName))
            sources.append(sourceName);
        for (const slm::Issue &issue : plan.issues) {
            const QString key = QStringLiteral("%1|%2|%3")
                                    .arg(static_cast<int>(issue.severity))
                                    .arg(issue.code, issue.detail);
            if (seen.contains(key))
                continue;
            seen.append(key);
            if (issue.isBlocker() && issue.code != QStringLiteral("windows-filesystem-ack")) {
                anyBlocker = true;
                if (!blockerSeen.contains(key)) {
                    blockerSeen.insert(key);
                    blockers.append(issue);
                }
            }
            issueText << QStringLiteral("%1 — %2: %3%4")
                             .arg(plan.game.name,
                                  issue.isBlocker() ? QStringLiteral("BLOCKER")
                                                    : QStringLiteral("Warning"),
                                  issue.message,
                                  issue.detail.isEmpty()
                                      ? QString()
                                      : QStringLiteral(" (%1)").arg(issue.detail));
        }
    }

    const auto blockerRank = [](const slm::Issue &issue) {
        if (issue.code.startsWith(QStringLiteral("ntfs-case-collision")))
            return 0;
        if (issue.code == QStringLiteral("source-readonly")
            || issue.code == QStringLiteral("destination-readonly"))
            return 1;
        if (issue.code == QStringLiteral("source-not-writable")
            || issue.code == QStringLiteral("destination-not-writable"))
            return 2;
        return 3;
    };
    std::stable_sort(blockers.begin(), blockers.end(), [&](const slm::Issue &a, const slm::Issue &b) {
        return blockerRank(a) < blockerRank(b);
    });
    bool hasCaseCollision = false;
    bool hasReadOnly = false;
    for (const slm::Issue &issue : blockers) {
        if (issue.code.startsWith(QStringLiteral("ntfs-case-collision")))
            hasCaseCollision = true;
        if (issue.code == QStringLiteral("source-readonly")
            || issue.code == QStringLiteral("destination-readonly")
            || issue.code == QStringLiteral("source-not-writable")
            || issue.code == QStringLiteral("destination-not-writable")) {
            hasReadOnly = true;
        }
    }
    QStringList guidance;
    if (hasCaseCollision) {
        guidance << QStringLiteral(
            "NTFS cannot store two names that differ only by case. Move to a case-sensitive "
            "Linux filesystem (Btrfs/ext4/XFS), or back up and resolve the conflicting source "
            "entry before retrying.");
    }
    if (hasReadOnly) {
        guidance << QStringLiteral(
            "A source or destination library is mounted read-only. Remount it read-write with "
            "the correct uid/gid before moving.");
    }
    if (guidance.isEmpty() && anyBlocker) {
        guidance << QStringLiteral("Review the blocker details below. No files have been changed.");
    }

    const slm::Library *destLibrary = libraryFor(m_batchDestination);
    const QString destinationName = destLibrary ? ui::driveDisplayName(*destLibrary)
                                                 : m_batchDestination;
    const quint64 destinationFree = destLibrary ? destLibrary->mount.freeBytes : 0;
    const quint64 destinationAfter = destinationFree > totalBytes ? destinationFree - totalBytes : 0;

    // Never rely on Breeze's Mid/Disabled colors for text; they can collapse into
    // the dark window background. Use an explicit, theme-aware muted color instead.
    const QString muted = ui::isDarkTheme() ? QStringLiteral("#aab4c0")
                                            : QStringLiteral("#66717d");
    const QString border = ui::isDarkTheme() ? QStringLiteral("#3d444d")
                                             : QStringLiteral("#c4cad2");
    const QString danger = ui::isDarkTheme() ? QStringLiteral("#ff9b93")
                                             : QStringLiteral("#a11b1b");
    const QString dangerBg = ui::isDarkTheme() ? QStringLiteral("#3a2222")
                                               : QStringLiteral("#fdecec");

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Review transfer"));

    // Keep the footer visible even on a short screen; only the body scrolls.
    const QRect available = screen() ? screen()->availableGeometry()
                                     : QRect(0, 0, 1280, 800);
    const int width = qMin(940, qMax(620, available.width() - 60));
    const int height = qMin(700, qMax(430, available.height() - 100));
    dialog.setMinimumSize(560, 400);
    dialog.resize(width, height);

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(8);
    // The body lives in a scroll area, so the dialog must be allowed to be
    // smaller than its content's natural size on short screens.
    root->setSizeConstraint(QLayout::SetNoConstraint);

    auto *heading = new QLabel(
        QStringLiteral("<b style='font-size:16px'>Review transfer</b><br>"
                       "<span style='color:%1'>Move %2 game%3 · %4 payload</span>")
            .arg(muted)
            .arg(m_batchPlans.size())
            .arg(m_batchPlans.size() == 1 ? QString() : QStringLiteral("s"))
            .arg(payloadBytes == 0 ? QStringLiteral("unknown size")
                                   : slm::formatBytes(payloadBytes)),
        &dialog);
    heading->setTextFormat(Qt::RichText);
    root->addWidget(heading);

    auto *scroll = new QScrollArea(&dialog);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setMinimumHeight(100);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(10);
    scroll->setWidget(body);
    root->addWidget(scroll, 1);

    if (anyBlocker) {
        auto *blocked = new QFrame(body);
        blocked->setObjectName(QStringLiteral("blockedBanner"));
        blocked->setStyleSheet(
            QStringLiteral("QFrame#blockedBanner { background: %1; border: 1px solid %2; "
                           "border-radius: 5px; }")
                .arg(dangerBg, danger));
        auto *blockedLayout = new QVBoxLayout(blocked);
        blockedLayout->setContentsMargins(12, 8, 12, 8);
        blockedLayout->setSpacing(3);
        auto *blockedTitle = new QLabel(
            QStringLiteral("<b>Move blocked by %1 blocker%2</b>")
                .arg(blockers.size())
                .arg(blockers.size() == 1 ? QString() : QStringLiteral("s")),
            blocked);
        blockedTitle->setTextFormat(Qt::RichText);
        blockedTitle->setStyleSheet(QStringLiteral("color: %1;").arg(danger));
        blockedLayout->addWidget(blockedTitle);
        QStringList lines;
        const int shown = qMin(3, blockers.size());
        for (int i = 0; i < shown; ++i) {
            const slm::Issue &issue = blockers.at(i);
            QString line = QStringLiteral("• <b>%1</b>").arg(issue.message.toHtmlEscaped());
            if (!issue.detail.isEmpty())
                line += QStringLiteral("<br><span style='color:%1'>%2</span>")
                            .arg(muted, issue.detail.toHtmlEscaped());
            lines << line;
        }
        if (blockers.size() > shown) {
            lines << QStringLiteral("• …and %1 more (see findings below)")
                          .arg(blockers.size() - shown);
        }
        auto *blockedReasons = new QLabel(lines.join(QStringLiteral("<br>")), blocked);
        blockedReasons->setTextFormat(Qt::RichText);
        blockedReasons->setWordWrap(true);
        blockedReasons->setTextInteractionFlags(Qt::TextSelectableByMouse);
        blockedReasons->setStyleSheet(QStringLiteral("color: %1;").arg(muted));
        blockedLayout->addWidget(blockedReasons);
        if (!guidance.isEmpty()) {
            auto *blockedHelp = new QLabel(guidance.join(QLatin1Char(' ')), blocked);
            blockedHelp->setWordWrap(true);
            blockedHelp->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
            blockedLayout->addWidget(blockedHelp);
        }
        layout->addWidget(blocked);
    }
    if (ntfsWarning) {
        auto *ntfsNotice = new QLabel(
            QStringLiteral("NTFS destination: tick the Windows-filesystem acknowledgement at "
                           "the bottom of this review before Start can be enabled."),
            body);
        ntfsNotice->setWordWrap(true);
        ntfsNotice->setStyleSheet(
            QStringLiteral("color: %1; background: %2; border: 1px solid %3; "
                           "border-radius: 4px; padding: 6px 8px; font-size: 10px;")
                .arg(ui::isDarkTheme() ? QStringLiteral("#f2c96d")
                                       : QStringLiteral("#7a5200"),
                     ui::isDarkTheme() ? QStringLiteral("#332b1b")
                                       : QStringLiteral("#fff7e0"),
                     ui::isDarkTheme() ? QStringLiteral("#6b5625")
                                       : QStringLiteral("#e3c26a")));
        layout->addWidget(ntfsNotice);
    }

    const auto makeCard = [&](const QString &kicker, const QString &title,
                              const QString &meta) {
        auto *card = new QFrame(body);
        card->setObjectName(QStringLiteral("reviewCard"));
        card->setStyleSheet(
            QStringLiteral("QFrame#reviewCard { background: palette(base); border: 1px solid %1; "
                           "border-radius: 5px; }")
                .arg(border));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 8, 12, 8);
        cardLayout->setSpacing(2);
        auto *kickerLabel = new QLabel(kicker, card);
        kickerLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 9px; font-weight: 600;").arg(muted));
        auto *titleLabel = new QLabel(title, card);
        titleLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
        auto *metaLabel = new QLabel(meta, card);
        metaLabel->setStyleSheet(
            QStringLiteral("color: %1; font-size: 10px;").arg(muted));
        metaLabel->setWordWrap(true);
        cardLayout->addWidget(kickerLabel);
        cardLayout->addWidget(titleLabel);
        cardLayout->addWidget(metaLabel);
        return card;
    };

    auto *route = new QHBoxLayout();
    route->setSpacing(8);
    route->addWidget(makeCard(QStringLiteral("FROM · %1 LIBRAR%2")
                                  .arg(sources.size())
                                  .arg(sources.size() == 1 ? QStringLiteral("Y") : QStringLiteral("IES")),
                              sources.join(QStringLiteral(" + ")),
                              QStringLiteral("Verified source libraries")),
                     1);
    auto *arrow = new QLabel(QStringLiteral("→"), body);
    arrow->setAlignment(Qt::AlignCenter);
    arrow->setStyleSheet(QStringLiteral("font-size: 18px; color: %1;").arg(muted));
    route->addWidget(arrow);
    route->addWidget(makeCard(QStringLiteral("TO"),
                              destinationName,
                              QStringLiteral("%1 free of %2 · ~%3 after")
                                  .arg(slm::formatBytes(destinationFree),
                                       slm::formatBytes(destLibrary ? destLibrary->mount.totalBytes : 0),
                                       slm::formatBytes(destinationAfter))),
                     1);
    layout->addLayout(route);

    auto *table = new QTableWidget(body);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({QStringLiteral("Game"), QStringLiteral("Payload")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setDefaultSectionSize(32);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->setRowCount(m_batchPlans.size());
    table->setIconSize(QSize(22, 30));
    for (int i = 0; i < m_batchPlans.size(); ++i) {
        const slm::Game &game = m_batchPlans.at(i).game;
        auto *nameItem = new QTableWidgetItem(game.name);
        if (!game.artworkPath.isEmpty()) {
            const QPixmap pixmap(game.artworkPath);
            if (!pixmap.isNull())
                nameItem->setIcon(QIcon(pixmap));
        }
        table->setItem(i, 0, nameItem);
        table->setItem(i, 1, new QTableWidgetItem(
            game.sizeOnDisk == 0 ? QStringLiteral("unknown")
                                 : slm::formatBytes(game.sizeOnDisk)));
    }
    const int tableHeader = table->horizontalHeader()->sizeHint().height();
    const int visibleRows = qMin(m_batchPlans.size(), 5);
    table->setFixedHeight(tableHeader + visibleRows * 32 + 4);
    layout->addWidget(table);

    auto *total = new QLabel(
        QStringLiteral("<span style='color:%1'>Total payload</span> &nbsp; "
                       "<b>%2</b>")
            .arg(muted,
                 payloadBytes == 0 ? QStringLiteral("unknown")
                                   : slm::formatBytes(payloadBytes)),
        body);
    total->setTextFormat(Qt::RichText);
    layout->addWidget(total);

    auto *spaceBox = new QFrame(body);
    spaceBox->setObjectName(QStringLiteral("spaceBox"));
    spaceBox->setStyleSheet(
        QStringLiteral("QFrame#spaceBox { background: palette(base); border: 1px solid %1; "
                       "border-radius: 5px; }")
            .arg(border));
    auto *spaceLayout = new QHBoxLayout(spaceBox);
    spaceLayout->setContentsMargins(12, 8, 12, 8);
    const auto addSpace = [&](const QString &label, const QString &value,
                              const QString &note) {
        auto *box = new QVBoxLayout();
        auto *lab = new QLabel(label, spaceBox);
        lab->setStyleSheet(
            QStringLiteral("color: %1; font-size: 9px; font-weight: 600;").arg(muted));
        auto *val = new QLabel(value, spaceBox);
        val->setStyleSheet(QStringLiteral("font-weight: 600;"));
        auto *fine = new QLabel(note, spaceBox);
        fine->setStyleSheet(QStringLiteral("color: %1; font-size: 9px;").arg(muted));
        fine->setWordWrap(true);
        box->addWidget(lab);
        box->addWidget(val);
        box->addWidget(fine);
        spaceLayout->addLayout(box, 1);
    };
    addSpace(QStringLiteral("DESTINATION NOW"), slm::formatBytes(destinationFree),
             destinationName);
    addSpace(QStringLiteral("NEEDED DURING COPY"), slm::formatBytes(totalBytes),
             QStringLiteral("payload + prefix/shadercache"));
    addSpace(QStringLiteral("AFTER CLEANUP"),
             QStringLiteral("~%1 free").arg(slm::formatBytes(destinationAfter)),
             QStringLiteral("source recovers ~%1").arg(slm::formatBytes(totalBytes)));
    layout->addWidget(spaceBox);

    auto *method = new QLabel(
        QStringLiteral("Copy  →  Verify  →  Update Steam  →  Clean up source"), body);
    method->setStyleSheet(
        QStringLiteral("color: %1; padding: 5px 8px; border: 1px solid %2; "
                       "border-radius: 3px;")
            .arg(muted, border));
    layout->addWidget(method);

    auto *safety = new QLabel(
        QStringLiteral("✓ The original files remain in place until copied data verifies and "
                       "the Steam manifest is updated. Cancelling before the commit keeps the "
                       "source intact."),
        body);
    safety->setWordWrap(true);
    safety->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    layout->addWidget(safety);

    auto *vdfNote = new QLabel(
        QStringLiteral("Only the moved games' existing entries in libraryfolders.vdf are updated, "
                       "with a backup. No new libraries or drive entries are added."),
        body);
    vdfNote->setWordWrap(true);
    vdfNote->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    layout->addWidget(vdfNote);

    if (!issueText.isEmpty()) {
        auto *issues = new QTextBrowser(body);
        issues->setMaximumHeight(92);
        issues->setStyleSheet(
            QStringLiteral("QTextBrowser { background: palette(base); border: 1px solid %1; "
                           "border-radius: 4px; }")
                .arg(border));
        issues->setPlainText(issueText.join(QLatin1Char('\n')));
        layout->addWidget(issues);
    }

    auto *fullVerify = new QCheckBox(QStringLiteral("Full checksum verification (slower)"), body);
    fullVerify->setChecked(m_batchOptions.fullVerify);
    layout->addWidget(fullVerify);
    auto *keepBackup = new QCheckBox(
        QStringLiteral("Keep a source backup (hidden .slm-backup-* folders)"), body);
    keepBackup->setChecked(m_batchOptions.keepSourceBackup);
    layout->addWidget(keepBackup);
    QCheckBox *ack = nullptr;
    if (ntfsWarning) {
        ack = new QCheckBox(
            QStringLiteral("I understand the destination is a Windows-compatible filesystem; "
                           "native Linux games may not run there."),
            body);
        layout->addWidget(ack);
    }

    auto *footer = new QHBoxLayout();
    auto *reviewNote = new QLabel(QStringLiteral("Review changes nothing until you start."),
                                  &dialog);
    reviewNote->setStyleSheet(
        QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    footer->addWidget(reviewNote);
    if (anyBlocker) {
        auto *why = new QPushButton(QStringLiteral("Why blocked?"), &dialog);
        why->setAutoDefault(false);
        why->setObjectName(QStringLiteral("whyBlocked"));
        connect(why, &QPushButton::clicked, &dialog, [&]() {
            QString html = QStringLiteral("<b>The move cannot start</b><br><br>");
            for (const slm::Issue &issue : blockers) {
                html += QStringLiteral("<b>%1</b><br>").arg(issue.message.toHtmlEscaped());
                if (!issue.detail.isEmpty())
                    html += QStringLiteral("%1<br>").arg(issue.detail.toHtmlEscaped());
                html += QStringLiteral("<br>");
            }
            if (!guidance.isEmpty()) {
                html += QStringLiteral("<b>What to do</b><br>");
                for (const QString &line : guidance)
                    html += QStringLiteral("• %1<br>").arg(line.toHtmlEscaped());
                html += QStringLiteral("<br>");
            }
            html += QStringLiteral("No files have been changed.");
            QMessageBox box(QMessageBox::Warning, QStringLiteral("Why is this blocked?"),
                            html, QMessageBox::Close, &dialog);
            box.setTextFormat(Qt::RichText);
            box.exec();
        });
        footer->addWidget(why);
    }
    footer->addStretch();
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), &dialog);
    auto *preflight = new QPushButton(QStringLiteral("Preflight only"), &dialog);
    auto *okButton = new QPushButton(QStringLiteral("Start transfer →"), &dialog);
    okButton->setObjectName(QStringLiteral("startTransfer"));
    okButton->setDefault(true);
    cancelButton->setAutoDefault(false);
    preflight->setAutoDefault(false);
    footer->addWidget(cancelButton);
    footer->addWidget(preflight);
    footer->addWidget(okButton);
    root->addLayout(footer);

    const auto updateEnabled = [&]() {
        okButton->setEnabled(!anyBlocker && (!ack || ack->isChecked()));
    };
    updateEnabled();
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(okButton, &QPushButton::clicked, &dialog, [&dialog, &okButton]() {
        if (okButton->isEnabled())
            dialog.accept();
    });
    if (ack)
        connect(ack, &QCheckBox::toggled, &dialog, [&](bool) { updateEnabled(); });
    connect(preflight, &QPushButton::clicked, &dialog, [&]() {
        QStringList what;
        what << QStringLiteral("%1 game(s), %2 total")
                    .arg(m_batchPlans.size())
                    .arg(slm::formatBytes(totalBytes));
        what << QStringLiteral("Destination: %1 (%2 free)")
                    .arg(destinationName, slm::formatBytes(destinationFree));
        what << QStringLiteral("No files have been changed. This is a preflight-only check.");
        if (anyBlocker)
            what.prepend(QStringLiteral("Preflight has blockers; Start is disabled."));
        QMessageBox::information(&dialog, QStringLiteral("Preflight only"),
                                 what.join(QLatin1Char('\n')));
    });

    if (dialog.exec() != QDialog::Accepted)
        return;

    for (slm::MovePlan &plan : m_batchPlans) {
        plan.options.fullVerify = fullVerify->isChecked();
        plan.options.allowNtfsNative = true;
        plan.options.keepSourceBackup = keepBackup->isChecked();
    }
    startMoveQueue(m_batchPlans);
}

void MainWindow::startMoveQueue(const QVector<slm::MovePlan> &plans)
{
    if (plans.isEmpty())
        return;
    m_batchPlans = plans;
    m_queueTotalBytes = 0;
    m_queueBaseBytes = 0;
    for (const slm::MovePlan &plan : plans)
        m_queueTotalBytes += plan.totalBytes;
    m_moveIndex = 0;
    m_batchResults.clear();
    m_queueFinished = false;
    m_queueCancelled = false;
    m_queuePaused = false;
    m_batchStopReason.clear();

    m_sleepInhibitor = std::make_unique<SleepInhibitor>();
    m_sleepInhibitor->inhibit(QStringLiteral("Moving Steam games"));

    m_moveProgress = new MoveProgressDialog(plans.size(), this);
    connect(m_moveProgress, &MoveProgressDialog::pauseToggled, this,
            &MainWindow::onPauseToggled);
    connect(m_moveProgress, &MoveProgressDialog::cancelRequested, this,
            &MainWindow::onCancelQueueRequested);
    m_moveProgress->show();
    startNextMove();
}

void MainWindow::startNextMove()
{
    if (m_queueFinished || m_moveThread)
        return;
    if (m_queueCancelled || m_moveIndex >= m_batchPlans.size()) {
        finishMoveQueue(m_queueCancelled ? QStringLiteral("Queue cancelled")
                                         : m_batchStopReason);
        return;
    }
    if (m_queuePaused) {
        m_moveProgress->setQueueMessage(
            QStringLiteral("Paused before the next game. Resume when ready."));
        return;
    }

    QString refreshError;
    if (!refreshRemainingSnapshots(&refreshError)) {
        m_batchStopReason = QStringLiteral("Could not refresh the library database snapshot: %1")
                                .arg(refreshError);
        finishMoveQueue(m_batchStopReason);
        return;
    }

    const slm::MovePlan &plan = m_batchPlans.at(m_moveIndex);
    m_queueBaseBytes = 0;
    for (int i = 0; i < m_moveIndex; ++i)
        m_queueBaseBytes += m_batchPlans.at(i).totalBytes;
    m_moveProgress->setPauseEnabled(true);
    const slm::Library *sourceLibrary = libraryFor(plan.game.libraryPath);
    const slm::Library *destLibrary = libraryFor(plan.destLibraryPath);
    m_moveProgress->setGame(
        m_moveIndex, m_batchPlans.size(), plan.game.name,
        sourceLibrary ? ui::driveDisplayName(*sourceLibrary) : plan.game.libraryPath,
        destLibrary ? ui::driveDisplayName(*destLibrary) : plan.destLibraryPath,
        plan.game.artworkPath);

    auto *thread = new MoveThread(plan, m_mounts, this);
    m_moveThread = thread;
    connect(thread, &MoveThread::progressChanged, this, &MainWindow::onMoveProgress);
    connect(thread, &MoveThread::moveFinished, this, &MainWindow::onMoveFinished);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_moveThread == thread)
            m_moveThread = nullptr;
        thread->deleteLater();
    });
    thread->start();
}

bool MainWindow::refreshRemainingSnapshots(QString *error)
{
    for (int i = m_moveIndex; i < m_batchPlans.size(); ++i) {
        QString refreshError;
        if (!slm::MovePlanner::refreshVdfSnapshot(m_batchPlans[i], &refreshError)) {
            if (error)
                *error = refreshError;
            return false;
        }
    }
    if (error)
        error->clear();
    return true;
}

void MainWindow::onMoveProgress(const slm::MoveProgress &progress)
{
    if (!m_moveProgress)
        return;
    m_moveProgress->setProgress(progress);
    m_moveProgress->setQueueProgress(m_queueBaseBytes + progress.bytesDone,
                                     m_queueTotalBytes);
    const bool commitPhase = progress.phase == slm::Phase::Verifying
        || progress.phase == slm::Phase::Syncing
        || progress.phase == slm::Phase::WritingManifest
        || progress.phase == slm::Phase::UpdatingVdf
        || progress.phase == slm::Phase::RemovingSourceManifest
        || progress.phase == slm::Phase::RemovingSourcePayload;
    if (commitPhase && m_queuePaused) {
        m_queuePaused = false;
        if (m_moveThread)
            m_moveThread->resume();
    }
    m_moveProgress->setPauseEnabled(!commitPhase);
}

void MainWindow::onMoveFinished(const slm::MoveResult &result)
{
    m_batchResults.append(result);
    m_moveThread = nullptr;
    ++m_moveIndex;

    if (!result.ok) {
        if (result.cancelled)
            m_queueCancelled = true;
        else
            m_batchStopReason = result.error;
    }
    if (m_queueCancelled || !result.ok) {
        finishMoveQueue(m_queueCancelled ? QStringLiteral("Queue cancelled")
                                         : m_batchStopReason);
        return;
    }

    QString refreshError;
    if (!refreshRemainingSnapshots(&refreshError)) {
        m_batchStopReason = QStringLiteral("Could not refresh the library database after the move: %1")
                                .arg(refreshError);
        finishMoveQueue(m_batchStopReason);
        return;
    }
    if (m_moveIndex >= m_batchPlans.size()) {
        finishMoveQueue(QString());
        return;
    }
    if (m_queuePaused) {
        m_moveProgress->setQueueMessage(
            QStringLiteral("Paused before the next game. Resume when ready."));
        return;
    }
    startNextMove();
}

void MainWindow::onPauseToggled(bool paused)
{
    m_queuePaused = paused;
    if (m_moveThread) {
        if (paused)
            m_moveThread->pause();
        else
            m_moveThread->resume();
    }
    if (m_moveProgress)
        m_moveProgress->setPaused(paused);
    if (!paused && !m_moveThread && !m_queueFinished && !m_queueCancelled)
        startNextMove();
}

void MainWindow::onCancelQueueRequested()
{
    if (m_queueFinished)
        return;
    m_queueCancelled = true;
    if (m_moveThread) {
        m_moveThread->cancel();
        if (m_moveProgress)
            m_moveProgress->setQueueMessage(
                QStringLiteral("Cancelling safely — source files are kept until the commit starts."));
    } else {
        finishMoveQueue(QStringLiteral("Queue cancelled"));
    }
}

void MainWindow::finishMoveQueue(const QString &stopReason)
{
    if (m_queueFinished)
        return;
    m_queueFinished = true;

    if (m_moveProgress) {
        m_moveProgress->finish();
        m_moveProgress->deleteLater();
        m_moveProgress = nullptr;
    }
    m_sleepInhibitor.reset();

    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    QStringList errors;
    QStringList details;
    QStringList backups;
    for (const slm::MoveResult &result : m_batchResults) {
        if (result.ok)
            ++succeeded;
        else if (result.cancelled)
            ++cancelled;
        else
            ++failed;
        if (!result.error.isEmpty())
            errors << result.error;
        if (!result.logPath.isEmpty())
            details << QStringLiteral("Log: %1").arg(result.logPath);
        if (!result.journalPath.isEmpty())
            details << QStringLiteral("Journal: %1").arg(result.journalPath);
        for (const QString &cleanup : result.cleanupErrors)
            errors << QStringLiteral("Cleanup: %1").arg(cleanup);
        for (const QString &backup : result.backupPaths)
            backups << backup;
    }

    const slm::Library *destLibrary = libraryFor(m_batchDestination);
    const QString destinationName = destLibrary ? ui::driveDisplayName(*destLibrary)
                                                 : m_batchDestination;
    const int attempted = m_batchResults.size();
    const int total = m_batchPlans.size();
    QString summary;
    if (succeeded == total && failed == 0 && cancelled == 0) {
        summary = QStringLiteral("Moved %1 game%2 to %3.")
                      .arg(succeeded)
                      .arg(succeeded == 1 ? QString() : QStringLiteral("s"))
                      .arg(destinationName);
    } else if (succeeded > 0) {
        summary = QStringLiteral("Moved %1 of %2 games to %3.")
                      .arg(succeeded)
                      .arg(total)
                      .arg(destinationName);
    } else {
        summary = QStringLiteral("No games were moved.");
    }
    if (cancelled > 0 && attempted < total)
        summary += QStringLiteral(" The queue was cancelled; %1 game(s) were not attempted.")
                       .arg(total - attempted);
    if (failed > 0)
        summary += QStringLiteral(" %1 move(s) failed.").arg(failed);
    if (!stopReason.isEmpty() && failed == 0 && cancelled == 0)
        summary += QStringLiteral(" %1").arg(stopReason);

    if (!backups.isEmpty())
        details << QStringLiteral("Source backups:") << backups;

    const QString notificationBody = summary;
    ui::sendDesktopNotification(QStringLiteral("Steam Library Mover"), notificationBody);

    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Steam Library Mover"));
    box.setIcon(failed > 0 ? QMessageBox::Warning
                           : (succeeded > 0 ? QMessageBox::Information : QMessageBox::Information));
    box.setText(summary);
    if (!errors.isEmpty())
        box.setDetailedText((details + errors).join(QLatin1Char('\n')));
    else if (!details.isEmpty())
        box.setDetailedText(details.join(QLatin1Char('\n')));
    QPushButton *startSteamButton = nullptr;
    if (succeeded > 0)
        startSteamButton = box.addButton(QStringLiteral("Start Steam"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (startSteamButton && box.clickedButton() == startSteamButton) {
        QString error;
        if (!slm::startSteam(m_scan.root.kind, &error))
            QMessageBox::warning(this, QStringLiteral("Could not start Steam"), error);
    }
    rescan();
}

void MainWindow::quitSteam()
{
    if (m_moveThread || m_moveProgress) {
        QMessageBox::information(this, QStringLiteral("Move in progress"),
                                 QStringLiteral("Do not start or stop Steam while a move queue is running."));
        return;
    }
    const slm::ProcessUsage usage = slm::inspectProcesses({}, {});
    if (!usage.steamRunning) {
        QMessageBox::information(this, QStringLiteral("Steam is not running"),
                                 QStringLiteral("Steam is already closed."));
        updateSteamBanner();
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Quit Steam"),
                              QStringLiteral("Ask Steam to shut down gracefully? Save any open games first."))
        != QMessageBox::Yes) {
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    const bool ok = slm::requestSteamShutdown(m_scan.root.kind, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Could not quit Steam"), error);
    }
    updateSteamBanner();
}

void MainWindow::showLibraryHealth()
{
    if (m_scanInProgress || m_scanIsCached) {
        QMessageBox::information(this, QStringLiteral("Refreshing library"),
                                 QStringLiteral("Wait for the cached library refresh to finish "
                                                "before running Library Health."));
        return;
    }
    if (m_moveThread || m_moveProgress) {
        QMessageBox::information(this, QStringLiteral("Move in progress"),
                                 QStringLiteral("Finish or cancel the move queue before running "
                                                "Library Health."));
        return;
    }
    if (m_scan.libraries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Library Health"),
                                 QStringLiteral("Scan a Steam installation first."));
        return;
    }

    QVector<slm::HealthFinding> findings;
    QString scanError;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    findings = slm::LibraryHealth::scan(m_scan, &scanError);
    QApplication::restoreOverrideCursor();
    if (!scanError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Library Health"), scanError);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Library Health"));
    dialog.resize(1160, 680);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(&dialog);
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    layout->addWidget(intro);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({QStringLiteral("Severity"), QStringLiteral("Type"),
                                      QStringLiteral("Library"), QStringLiteral("Path"),
                                      QStringLiteral("Detail")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table->verticalHeader()->setVisible(false);
    table->setWordWrap(false);
    table->verticalHeader()->setDefaultSectionSize(28);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    layout->addWidget(table, 1);

    auto *note = new QLabel(
        QStringLiteral("Quarantine moves selected lock/temp files into a hidden "
                       "<code>.slm-quarantine</code> folder next to the original. Nothing is "
                       "deleted. Steam and all Proton/Wine games must be closed."),
        &dialog);
    note->setWordWrap(true);
    note->setTextFormat(Qt::RichText);
    setMutedLabel(note);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *rescanButton = buttons->addButton(QStringLiteral("Rescan"), QDialogButtonBox::ActionRole);
    auto *selectSafeButton =
        buttons->addButton(QStringLiteral("Select lock/temp files"), QDialogButtonBox::ActionRole);
    auto *quarantineButton =
        buttons->addButton(QStringLiteral("Quarantine selected…"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);

    const auto severityColor = [](slm::HealthSeverity severity) {
        if (severity == slm::HealthSeverity::Critical)
            return ui::isDarkTheme() ? QColor(255, 107, 107) : QColor(180, 30, 30);
        if (severity == slm::HealthSeverity::Warning)
            return ui::isDarkTheme() ? QColor(255, 184, 77) : QColor(160, 100, 0);
        return QColor();
    };
    const auto populate = [&]() {
        int critical = 0;
        int warnings = 0;
        int infos = 0;
        int actionable = 0;
        for (const slm::HealthFinding &finding : findings) {
            if (finding.severity == slm::HealthSeverity::Critical)
                ++critical;
            else if (finding.severity == slm::HealthSeverity::Warning)
                ++warnings;
            else
                ++infos;
            if (finding.quarantineable)
                ++actionable;
        }
        intro->setText(
            QStringLiteral("<b>%1 critical, %2 warnings, %3 informational findings</b><br>"
                           "%4 lock/temp file(s) can be quarantined. Source backups are listed "
                           "for visibility and are never deleted automatically.")
                .arg(critical)
                .arg(warnings)
                .arg(infos)
                .arg(actionable));
        table->clearContents();
        table->setRowCount(findings.size());
        for (int row = 0; row < findings.size(); ++row) {
            const slm::HealthFinding &finding = findings.at(row);
            auto *severityItem =
                new QTableWidgetItem(slm::LibraryHealth::severityName(finding.severity));
            if (severityColor(finding.severity).isValid())
                severityItem->setForeground(severityColor(finding.severity));
            table->setItem(row, 0, severityItem);
            table->setItem(row, 1, new QTableWidgetItem(slm::LibraryHealth::kindName(finding.kind)));
            const slm::Library *library = libraryFor(finding.libraryPath);
            table->setItem(row, 2, new QTableWidgetItem(
                library ? ui::driveDisplayName(*library) : finding.libraryPath));
            auto *pathItem = new QTableWidgetItem(finding.path);
            pathItem->setToolTip(finding.path);
            table->setItem(row, 3, pathItem);
            auto *detailItem = new QTableWidgetItem(finding.title
                                                    + (finding.detail.isEmpty()
                                                           ? QString()
                                                           : QStringLiteral(" — ") + finding.detail));
            detailItem->setToolTip(finding.detail);
            table->setItem(row, 4, detailItem);
            for (int column = 0; column < 5; ++column)
                table->item(row, column)->setData(Qt::UserRole, row);
        }
        quarantineButton->setEnabled(false);
        selectSafeButton->setEnabled(actionable > 0);
    };
    const auto selectedQuarantinable = [&]() {
        QVector<slm::HealthFinding> selected;
        if (!table->selectionModel())
            return selected;
        const QModelIndexList rows = table->selectionModel()->selectedRows();
        QSet<QString> seen;
        for (const QModelIndex &index : rows) {
            const int row = index.row();
            if (row < 0 || row >= findings.size() || !findings.at(row).quarantineable)
                continue;
            const QString path = findings.at(row).path;
            if (seen.contains(path))
                continue;
            seen.insert(path);
            selected.append(findings.at(row));
        }
        return selected;
    };
    populate();
    QObject::connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, &dialog,
                     [&]() { quarantineButton->setEnabled(!selectedQuarantinable().isEmpty()); });
    QObject::connect(selectSafeButton, &QPushButton::clicked, &dialog, [&]() {
        if (!table->selectionModel())
            return;
        table->clearSelection();
        for (int row = 0; row < findings.size(); ++row) {
            if (!findings.at(row).quarantineable)
                continue;
            const QModelIndex index = table->model()->index(row, 0);
            table->selectionModel()->select(index,
                                            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    });
    QObject::connect(quarantineButton, &QPushButton::clicked, &dialog, [&]() {
        const QVector<slm::HealthFinding> selected = selectedQuarantinable();
        if (selected.isEmpty()) {
            QMessageBox::information(&dialog, QStringLiteral("Nothing selected"),
                                     QStringLiteral("Select one or more lock or temp files."));
            return;
        }
        if (QMessageBox::question(
                &dialog, QStringLiteral("Quarantine files"),
                QStringLiteral("Move %1 selected file(s) into hidden .slm-quarantine folders?\n\n"
                               "Steam and all Proton/Wine games must be closed. Nothing is deleted; "
                               "you can restore a file from its quarantine folder.")
                    .arg(selected.size()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            != QMessageBox::Yes)
            return;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = slm::LibraryHealth::quarantine(m_scan, selected, &error);
        QApplication::restoreOverrideCursor();
        if (!ok)
            QMessageBox::warning(&dialog, QStringLiteral("Quarantine"), error);
        else
            QMessageBox::information(
                &dialog, QStringLiteral("Quarantine"),
                QStringLiteral("Moved %1 file(s) into .slm-quarantine folders.").arg(selected.size()));
        scanError.clear();
        findings = slm::LibraryHealth::scan(m_scan, &scanError);
        if (!scanError.isEmpty())
            QMessageBox::warning(&dialog, QStringLiteral("Library Health"), scanError);
        populate();
    });
    QObject::connect(rescanButton, &QPushButton::clicked, &dialog, [&]() {
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        findings = slm::LibraryHealth::scan(m_scan, &error);
        QApplication::restoreOverrideCursor();
        if (!error.isEmpty())
            QMessageBox::warning(&dialog, QStringLiteral("Library Health"), error);
        populate();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::showDoctor()
{
    if (m_scanInProgress || m_scanIsCached) {
        QMessageBox::information(this, QStringLiteral("Refreshing library"),
                                 QStringLiteral("Wait for the cached library refresh to finish "
                                                "before opening NTFS Doctor."));
        return;
    }
    if (m_moveThread || m_moveProgress) {
        QMessageBox::information(this, QStringLiteral("Move in progress"),
                                 QStringLiteral("Close the move queue before changing library configuration."));
        return;
    }
    if (m_scan.libraries.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("NTFS Doctor"),
                                 QStringLiteral("Scan a Steam installation first."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("NTFS Doctor"));
    dialog.resize(1120, 620);
    dialog.setMinimumWidth(1000);
    int windowsLibraries = 0;
    int configuredSymlinks = 0;
    int missingSymlinks = 0;
    int brokenSymlinks = 0;
    int realDirectories = 0;
    int wrongTargets = 0;
    QStringList compatTargets;
    for (const slm::Library &library : m_scan.libraries) {
        if (!library.mount.isWindowsCompatible())
            continue;
        ++windowsLibraries;
        switch (library.compatdata.state) {
        case slm::CompatDataState::Symlink:
            if (library.compatdata.targetOnWindowsFs) {
                ++wrongTargets;
            } else {
                ++configuredSymlinks;
                if (!compatTargets.contains(library.compatdata.target))
                    compatTargets.append(library.compatdata.target);
            }
            break;
        case slm::CompatDataState::Missing:
            ++missingSymlinks;
            break;
        case slm::CompatDataState::BrokenSymlink:
            ++brokenSymlinks;
            break;
        case slm::CompatDataState::Directory:
            ++realDirectories;
            break;
        }
    }
    QString introText;
    if (windowsLibraries == 0) {
        introText = QStringLiteral(
            "No Windows-compatible Steam libraries were found. No compatdata symlink is required.");
    } else if (configuredSymlinks == windowsLibraries) {
        QString targetHtml;
        for (const QString &target : compatTargets) {
            if (!targetHtml.isEmpty())
                targetHtml += QStringLiteral("<br>");
            targetHtml += QStringLiteral("<code>%1</code>").arg(target.toHtmlEscaped());
        }
        introText = QStringLiteral(
                        "<b>✔ All %1 Windows-filesystem libraries already use a compatdata "
                        "symlink.</b><br>Proton prefixes are stored on the Linux filesystem at "
                        "%2. No compatdata action is needed; the warnings below are only about "
                        "executable bits and mount options.")
                        .arg(windowsLibraries)
                        .arg(targetHtml);
    } else {
        introText =
            QStringLiteral("<b>⚠ %1 of %2 Windows-filesystem libraries need compatdata "
                           "attention.</b><br>%3 missing, %4 broken, %5 real directories, %6 "
                           "targets on a Windows filesystem. Use <i>Fix selected compatdata</i>.")
                .arg(configuredSymlinks)
                .arg(windowsLibraries)
                .arg(missingSymlinks)
                .arg(brokenSymlinks)
                .arg(realDirectories)
                .arg(wrongTargets);
    }
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(&dialog);
    intro->setTextFormat(Qt::RichText);
    intro->setWordWrap(true);
    intro->setText(introText);
    layout->addWidget(intro);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({QStringLiteral("Library"), QStringLiteral("Filesystem"),
                                      QStringLiteral("Compatdata"), QStringLiteral("Writable"),
                                      QStringLiteral("Mount options")});
    table->setRowCount(m_scan.libraries.size());
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->setColumnWidth(0, 330);
    table->setColumnWidth(2, 220);
    for (int row = 0; row < m_scan.libraries.size(); ++row) {
        const slm::Library &library = m_scan.libraries.at(row);
        auto *libraryItem = new QTableWidgetItem(library.rawPath);
        libraryItem->setToolTip(library.rawPath);
        table->setItem(row, 0, libraryItem);
        table->setItem(row, 1, new QTableWidgetItem(library.mount.fsType));
        auto *compatItem = new QTableWidgetItem(compatDataDoctorCell(library));
        compatItem->setToolTip(compatDataDescription(library));
        table->setItem(row, 2, compatItem);
        auto *writableItem = new QTableWidgetItem(library.writeAccess ? QStringLiteral("yes")
                                                                      : QStringLiteral("no"));
        writableItem->setToolTip(library.writeAccess
                                     ? QStringLiteral("Writable")
                                     : QStringLiteral("Not writable; check mount uid/gid/umask"));
        table->setItem(row, 3, writableItem);
        auto *optionsItem =
            new QTableWidgetItem(library.mount.options.join(QStringLiteral(", ")));
        optionsItem->setToolTip(library.mount.options.join(QStringLiteral(", ")));
        table->setItem(row, 4, optionsItem);
    }
    layout->addWidget(table, 1);

    auto *issues = new QTextBrowser(&dialog);
    issues->setPlainText(issueLines(slm::NtfsDoctor::inspect(m_scan)));
    layout->addWidget(new QLabel(QStringLiteral("<b>Findings</b>"), &dialog));
    layout->addWidget(issues);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *fix = buttons->addButton(QStringLiteral("Fix selected compatdata…"),
                                   QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(fix, &QPushButton::clicked, &dialog, [&, this]() {
        const int row = table->currentRow();
        if (row < 0) {
            QMessageBox::information(&dialog, QStringLiteral("NTFS Doctor"),
                                     QStringLiteral("Select a library first."));
            return;
        }
        const slm::Library &library = m_scan.libraries.at(row);
        if (!library.mount.isWindowsCompatible()) {
            QMessageBox::information(
                &dialog, QStringLiteral("NTFS Doctor"),
                QStringLiteral("This library is already on a Linux filesystem; compatdata does "
                               "not need a symlink here."));
            return;
        }
        if (library.compatdata.state == slm::CompatDataState::Symlink
            && !library.compatdata.targetOnWindowsFs) {
            QMessageBox::information(
                &dialog, QStringLiteral("Already configured"),
                QStringLiteral("This library already uses a valid shared compatdata symlink:\n%1\n\n"
                               "No changes are needed.")
                    .arg(library.compatdata.target));
            return;
        }
        const QString target = QInputDialog::getText(
            &dialog, QStringLiteral("Compatdata target"),
            QStringLiteral("Symlink\n%1\nto a directory on a Linux filesystem:")
                .arg(library.compatdataPath()),
            QLineEdit::Normal, slm::NtfsDoctor::defaultCompatDataTarget(m_scan));
        if (target.isEmpty())
            return;
        if (slm::inspectProcesses({}, {}).steamRunning) {
            QMessageBox::warning(&dialog, QStringLiteral("Steam is running"),
                                 QStringLiteral("Quit Steam before changing the library configuration."));
            return;
        }
        QString error;
        if (!slm::NtfsDoctor::fixCompatData(library, target, m_mounts, true, &error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Could not fix compatdata"), error);
            return;
        }
        QMessageBox::information(&dialog, QStringLiteral("Compatdata configured"),
                                 QStringLiteral("%1 -> %2").arg(library.compatdataPath(), target));
        dialog.accept();
    });
    layout->addWidget(buttons);
    dialog.exec();
    rescan();
}

void MainWindow::showRecovery()
{
    if (m_moveThread || m_moveProgress) {
        QMessageBox::information(this, QStringLiteral("Move in progress"),
                                 QStringLiteral("Finish or cancel the current queue before opening Recovery."));
        return;
    }
    const QList<QJsonObject> operations = slm::MoveExecutor::unfinishedOperations();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Recovery"));
    dialog.resize(900, 480);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral(
                                 "These operations did not reach a clean finished state. A rollback is only "
                                 "safe before the source manifest was removed. Review the log first."),
                             &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *table = new QTableWidget(&dialog);
    table->setColumnCount(6);
    table->setHorizontalHeaderLabels({QStringLiteral("Operation"), QStringLiteral("State"),
                                      QStringLiteral("Phase"), QStringLiteral("App"),
                                      QStringLiteral("Name"), QStringLiteral("Updated")});
    table->setRowCount(operations.size());
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    for (int row = 0; row < operations.size(); ++row) {
        const QJsonObject &operation = operations.at(row);
        table->setItem(row, 0, new QTableWidgetItem(operation.value(QStringLiteral("opId")).toString()));
        table->setItem(row, 1, new QTableWidgetItem(operation.value(QStringLiteral("state")).toString()));
        table->setItem(row, 2, new QTableWidgetItem(operation.value(QStringLiteral("phase")).toString()));
        table->setItem(row, 3, new QTableWidgetItem(QString::number(
            operation.value(QStringLiteral("appId")).toInteger())));
        table->setItem(row, 4, new QTableWidgetItem(operation.value(QStringLiteral("name")).toString()));
        table->setItem(row, 5, new QTableWidgetItem(operation.value(QStringLiteral("updatedAt")).toString()));
    }
    layout->addWidget(table, 1);
    if (operations.isEmpty())
        intro->setText(QStringLiteral("No unfinished operations were found."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *rollback = buttons->addButton(QStringLiteral("Roll back selected"),
                                         QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(rollback, &QPushButton::clicked, &dialog, [&]() {
        const int row = table->currentRow();
        if (row < 0 || row >= operations.size()) {
            QMessageBox::information(&dialog, QStringLiteral("Recovery"),
                                     QStringLiteral("Select an operation first."));
            return;
        }
        if (QMessageBox::question(&dialog, QStringLiteral("Roll back"),
                                  QStringLiteral("Remove the partial destination data and restore the "
                                                 "original library database? Source data is never removed "
                                                 "by a rollback."))
            != QMessageBox::Yes)
            return;
        QString error;
        if (!slm::MoveExecutor::rollbackOperation(operations.at(row), &error)) {
            QMessageBox::critical(&dialog, QStringLiteral("Rollback failed"), error);
            return;
        }
        QMessageBox::information(&dialog, QStringLiteral("Rolled back"),
                                 QStringLiteral("The partial operation was rolled back."));
        dialog.accept();
    });
    layout->addWidget(buttons);
    dialog.exec();
    rescan();
}

void MainWindow::promptUnfinishedOperations()
{
    if (slm::MoveExecutor::unfinishedOperations().isEmpty())
        return;
    QMessageBox::warning(this, QStringLiteral("Unfinished operation found"),
                         QStringLiteral("A previous move did not finish cleanly. Open Recovery to inspect "
                                        "and roll it back if needed."));
    showRecovery();
}

void MainWindow::showSettings()
{
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *themeCombo = new QComboBox(&dialog);
    themeCombo->addItem(QStringLiteral("Follow system theme"), QStringLiteral("system"));
    themeCombo->addItem(QStringLiteral("Light"), QStringLiteral("light"));
    themeCombo->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    const QString currentTheme =
        settings.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
    const int themeIndex = themeCombo->findData(currentTheme);
    if (themeIndex >= 0)
        themeCombo->setCurrentIndex(themeIndex);
    auto *rootEdit = new QLineEdit(&dialog);
    rootEdit->setPlaceholderText(QStringLiteral("Automatic detection"));
    rootEdit->setText(settings.value(QStringLiteral("steam/root")).toString());
    auto *rootForm = new QFormLayout();
    rootForm->addRow(QStringLiteral("Theme:"), themeCombo);
    auto *themeNote = new QLabel(
        QStringLiteral("The window title bar is drawn by KDE and follows your system window "
                       "decoration. The app's menus, toolbar and content follow the selected "
                       "theme."),
        &dialog);
    themeNote->setWordWrap(true);
    setMutedLabel(themeNote);
    rootForm->addRow(QString(), themeNote);
    rootForm->addRow(QStringLiteral("Steam root override:"), rootEdit);
    layout->addLayout(rootForm);
    auto *compact = new QCheckBox(
        QStringLiteral("Compact game rows (smaller artwork and title, more games visible)"), &dialog);
    compact->setChecked(settings.value(QStringLiteral("appearance/compact"), true).toBool());
    layout->addWidget(compact);
    auto *verify = new QCheckBox(QStringLiteral("Use full SHA-256 verification by default (slower)"),
                                 &dialog);
    verify->setChecked(settings.value(QStringLiteral("verify/full"), false).toBool());
    auto *shader = new QCheckBox(QStringLiteral("Move shader cache when present"), &dialog);
    shader->setChecked(settings.value(QStringLiteral("components/shadercache"), true).toBool());
    auto *workshop = new QCheckBox(QStringLiteral("Move Workshop content and manifest when present"),
                                   &dialog);
    workshop->setChecked(settings.value(QStringLiteral("components/workshop"), true).toBool());
    auto *artwork = new QCheckBox(
        QStringLiteral("Download missing box art from Steam's public CDN (cached locally)"), &dialog);
    artwork->setChecked(settings.value(QStringLiteral("artwork/download"), true).toBool());
    layout->addWidget(verify);
    layout->addWidget(shader);
    layout->addWidget(workshop);
    layout->addWidget(artwork);
    auto *note = new QLabel(QStringLiteral(
                                "Steam must be closed for every move. The mover never runs Steam's own "
                                "move operation and never deletes source data before verification."),
                            &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        settings.setValue(QStringLiteral("appearance/theme"), themeCombo->currentData().toString());
        ui::applyTheme(ui::themeFromName(themeCombo->currentData().toString()));
        refreshTheme();
        settings.setValue(QStringLiteral("steam/root"), rootEdit->text().trimmed());
        settings.setValue(QStringLiteral("appearance/compact"), compact->isChecked());
        applyDensity();
        settings.setValue(QStringLiteral("verify/full"), verify->isChecked());
        settings.setValue(QStringLiteral("components/shadercache"), shader->isChecked());
        settings.setValue(QStringLiteral("components/workshop"), workshop->isChecked());
        settings.setValue(QStringLiteral("artwork/download"), artwork->isChecked());
        requestArtworkForScan();
    }
}

void MainWindow::showAbout()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("About Steam Library Mover"));
    box.setIconPixmap(windowIcon().pixmap(64, 64));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral(
        "<h3>Steam Library Mover %1</h3>"
        "<p>Move installed Steam games between Steam libraries without using Steam's "
        "own “Move install folder” operation.</p>"
        "<p>Requires Steam to be closed. Supports native and Flatpak Steam libraries, "
        "NTFS compatdata symlinks, journaled moves and verification.</p>"
        "<p><b>Project:</b> <a href=\"https://github.com/NasirA1991/steam-library-mover\">"
        "github.com/NasirA1991/steam-library-mover</a><br>"
        "<b>AUR:</b> <a href=\"https://aur.archlinux.org/packages/steam-library-mover\">"
        "aur.archlinux.org/packages/steam-library-mover</a><br>"
        "<b>Issues:</b> <a href=\"https://github.com/NasirA1991/steam-library-mover/issues\">"
        "github.com/NasirA1991/steam-library-mover/issues</a></p>"
        "<p>GPL-3.0-or-later. Not affiliated with Valve Corporation. Steam is a "
        "trademark of Valve Corporation.</p>")
                  .arg(QApplication::applicationVersion()));
    box.setTextInteractionFlags(Qt::TextBrowserInteraction);
    if (auto *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"))) {
        label->setOpenExternalLinks(false);
        connect(label, &QLabel::linkActivated, this, [](const QString &url) {
            QDesktopServices::openUrl(QUrl(url));
        });
    }
    box.setStandardButtons(QMessageBox::Close);
    box.exec();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_moveThread) {
        if (QMessageBox::question(this, QStringLiteral("Move in progress"),
                                  QStringLiteral("A move is running. Cancel it and close? The source data "
                                                 "will not be removed, but a partial destination copy may "
                                                 "remain."))
            != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        m_moveThread->cancel();
        m_moveThread->wait();
    }
    if (m_scanThread)
        m_scanThread->wait();
    if (m_planThread)
        m_planThread->wait();
    for (const QPointer<SizeThread> &thread : m_sizeThreads) {
        if (thread)
            thread->wait();
    }
    event->accept();
}
