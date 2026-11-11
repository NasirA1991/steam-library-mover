// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/dragsupport.h"
#include "gui/gamemodel.h"
#include "gui/mainwindow.h"
#include "core/scancache.h"

#include "core/process_check.h"

#include "test_helpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QTableView>
#include <QTableWidget>
#include <QToolButton>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include <unistd.h>

class GuiTest : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> m_configDir;

private slots:
    void initTestCase();
    void startupAndSelection();
    void checkboxesDetailsAndTransferStrip();
    void dragDropTarget();
    void healthDialogOpens();
    void batchMoveFlow();
};

void GuiTest::initTestCase()
{
    if (::geteuid() == 0)
        QSKIP("GUI move tests must run as a normal user");
    m_configDir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_configDir->isValid());
    qputenv("XDG_CONFIG_HOME", m_configDir->path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_configDir->path());
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, m_configDir->path());
    QCoreApplication::setOrganizationName(QStringLiteral("SteamLibraryMoverTests"));
    QCoreApplication::setApplicationName(QStringLiteral("tst_gui"));
    slm::setProcessInspectorForTesting([](const QStringList &, const QStringList &) { return slm::ProcessUsage{}; });
}

void GuiTest::startupAndSelection()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    qputenv("SLM_STEAM_ROOT", steamRoot.toUtf8());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    qputenv("SLM_CACHE_DIR", (temp.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("SLM_DISABLE_ARTWORK", "1");

    MainWindow window;
    window.show();
    auto *table = window.findChild<QTableView *>();
    QVERIFY(table);
    QTRY_VERIFY_WITH_TIMEOUT(table->model()
                                 && table->model()->rowCount() > 0,
                             8000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isScanning(), 8000);
    QCOMPARE(table->model()->rowCount(), 1);
    auto *model = qobject_cast<GameTableModel *>(table->model());
    QCOMPARE(model->index(0, GameTableModel::StatusColumn).data().toString(), QString());
    QVERIFY(model);
    model->setRuntimeFilter(GameTableModel::RuntimeFilter::Proton);
    QCOMPARE(model->rowCount(), 0);
    model->setRuntimeFilter(GameTableModel::RuntimeFilter::Native);
    QCOMPARE(model->rowCount(), 0);
    model->setRuntimeFilter(GameTableModel::RuntimeFilter::Auto);
    QCOMPARE(model->rowCount(), 1);
    model->setStatusFilter(GameTableModel::StatusFilter::Blocked);
    QCOMPARE(model->rowCount(), 0);
    model->setStatusFilter(GameTableModel::StatusFilter::All);
    QCOMPARE(model->rowCount(), 1);

    const QModelIndex driveIndex =
        table->model()->index(0, GameTableModel::DriveColumn);
    QVERIFY(!driveIndex.data(Qt::DisplayRole).toString().isEmpty());
    QVERIFY(driveIndex.data(Qt::ToolTipRole).toString().contains(QStringLiteral("libraryA")));
    QComboBox *destination = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (combo->findData(slm::canonicalOrAbsolute(destLibrary)) >= 0) {
            destination = combo;
            break;
        }
    }
    QVERIFY(destination);
    QCOMPARE(destination->currentData().toString(), QString());
    table->selectRow(0);
    QVERIFY(!window.findChild<QPushButton *>()->isEnabled());
    destination->setCurrentIndex(
        destination->findData(slm::canonicalOrAbsolute(destLibrary)));
    QTRY_VERIFY(window.findChild<QPushButton *>()
                && window.findChild<QPushButton *>()->isEnabled());
    QVERIFY(destination->currentText().contains(QStringLiteral("free")));
    bool sizeDetailsReady = false;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (QLabel *label : window.findChildren<QLabel *>()) {
                if (label->text().contains(QStringLiteral("prefix"))
                    && !label->text().contains(QStringLiteral("calculating"))) {
                    sizeDetailsReady = true;
                    return true;
                }
            }
            return false;
        }(),
        5000);
    QVERIFY(sizeDetailsReady);
    QVERIFY(window.grab().save(QDir::currentPath() + QStringLiteral("/tst_gui.png")));
    window.close();
    QVERIFY(QFileInfo::exists(
        slm::ScanCache::cachePath({steamRoot, slm::InstallKind::Native})));
}


void GuiTest::checkboxesDetailsAndTransferStrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    qputenv("SLM_STEAM_ROOT", steamRoot.toUtf8());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    qputenv("SLM_CACHE_DIR", (temp.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("SLM_DISABLE_ARTWORK", "1");

    MainWindow window;
    window.show();
    auto *table = window.findChild<QTableView *>();
    QVERIFY(table);
    QTRY_VERIFY_WITH_TIMEOUT(table->model() && table->model()->rowCount() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isScanning(), 8000);
    auto *model = qobject_cast<GameTableModel *>(table->model());
    QVERIFY(model);

    const QModelIndex check = model->index(0, GameTableModel::CheckColumn);
    QCOMPARE(check.data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    table->scrollTo(check);
    const auto indicatorPoint = [&]() {
        const QRect rect = table->visualRect(check);
        return QPoint(rect.left() + 9, rect.center().y());
    };
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, indicatorPoint());
    QTRY_COMPARE(check.data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, indicatorPoint());
    QTRY_COMPARE(check.data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Unchecked));
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, indicatorPoint());
    QTRY_COMPARE(check.data(Qt::CheckStateRole).toInt(), static_cast<int>(Qt::Checked));

    auto *summary = window.findChild<QLabel *>(QStringLiteral("transferSummary"));
    QVERIFY(summary);
    QVERIFY(summary->text().contains(QStringLiteral("1 game selected")));
    auto *full = window.findChild<QLabel *>(QStringLiteral("detailsFull"));
    auto *toggle = window.findChild<QToolButton *>(QStringLiteral("detailsToggle"));
    QVERIFY(full && toggle);
    QVERIFY(!full->isVisible());
    toggle->click();
    QTRY_VERIFY(full->isVisible());
    toggle->click();
    QTRY_VERIFY(!full->isVisible());

    QVERIFY(model->setData(check, Qt::Unchecked, Qt::CheckStateRole));
    QCOMPARE(model->checkedAppIds().size(), 0);
    QVERIFY(summary->text().contains(QStringLiteral("Select games")));

    QVERIFY(model->setData(check, Qt::Checked, Qt::CheckStateRole));
    QCOMPARE(model->checkedAppIds().size(), 1);
    QVERIFY(summary->text().contains(QStringLiteral("1 game selected")));

    QComboBox *destination = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (combo->findData(slm::canonicalOrAbsolute(destLibrary)) >= 0) {
            destination = combo;
            break;
        }
    }
    QVERIFY(destination);
    destination->setCurrentIndex(destination->findData(slm::canonicalOrAbsolute(destLibrary)));
    auto *review = window.findChild<QPushButton *>(QStringLiteral("reviewButton"));
    QVERIFY(review);
    QTRY_VERIFY(review->isEnabled());

    auto *clear = window.findChild<QToolButton *>(QStringLiteral("clearSelection"));
    QVERIFY(clear);
    clear->click();
    QCOMPARE(model->checkedAppIds().size(), 0);
    QVERIFY(summary->text().contains(QStringLiteral("Select games")));
    window.close();
}

void GuiTest::dragDropTarget()
{
    LibraryListWidget list;
    list.resize(320, 240);
    auto *item = new QListWidgetItem(QStringLiteral("Library B"));
    item->setData(Qt::UserRole, QStringLiteral("/tmp/libraryB"));
    list.addItem(item);
    list.show();
    QTest::qWait(30);

    QString receivedPath;
    QList<quint32> receivedIds;
    QObject::connect(&list, &LibraryListWidget::gamesDroppedOnLibrary, &list,
                     [&](const QString &path, const QList<quint32> &ids) {
                         receivedPath = path;
                         receivedIds = ids;
                     });

    QMimeData mime;
    mime.setData("application/x-steam-library-mover-appids", "12345,67890");
    const QPoint pos = list.visualItemRect(item).center();
    QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list.viewport(), &enter);
    QDragMoveEvent move(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list.viewport(), &move);
    QDropEvent event(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(list.viewport(), &event);
    QCOMPARE(receivedPath, QStringLiteral("/tmp/libraryB"));
    QCOMPARE(receivedIds, QList<quint32>({12345, 67890}));
}

void GuiTest::healthDialogOpens()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    qputenv("SLM_STEAM_ROOT", steamRoot.toUtf8());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    qputenv("SLM_CACHE_DIR", (temp.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("SLM_DISABLE_ARTWORK", "1");

    MainWindow window;
    window.show();
    auto *table = window.findChild<QTableView *>();
    QVERIFY(table);
    QTRY_VERIFY_WITH_TIMEOUT(table->model() && table->model()->rowCount() == 1, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isScanning(), 8000);

    bool opened = false;
    QTimer watcher;
    watcher.setInterval(25);
    QObject::connect(&watcher, &QTimer::timeout, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (dialog && dialog->isVisible()
                && dialog->windowTitle() == QStringLiteral("Library Health")) {
                opened = true;
                dialog->accept();
            }
        }
    });
    watcher.start();
    QVERIFY(QMetaObject::invokeMethod(&window, "showLibraryHealth", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(opened, 5000);
    watcher.stop();
    window.close();
}

void GuiTest::batchMoveFlow()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString steamRoot = testutil::makeFixtureSteam(temp);
    const QString sourceLibrary = temp.path() + QStringLiteral("/libraryA");
    const QString destLibrary = temp.path() + QStringLiteral("/libraryB");
    QDir().mkpath(sourceLibrary + QStringLiteral("/steamapps/common/Test Game 2"));
    QVERIFY(testutil::writeFile(sourceLibrary + QStringLiteral("/steamapps/common/Test Game 2/game.dat"),
                                QByteArray(4096, 'B')));
    QVERIFY(testutil::writeFile(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12346.acf"),
                                testutil::manifestText(12346, "Test Game 2", "Test Game 2", 4, 4096)
                                    .toUtf8()));
    qputenv("SLM_STEAM_ROOT", steamRoot.toUtf8());
    qputenv("SLM_STATE_DIR", (temp.path() + QStringLiteral("/state")).toUtf8());
    qputenv("SLM_CACHE_DIR", (temp.path() + QStringLiteral("/cache")).toUtf8());
    qputenv("SLM_DISABLE_ARTWORK", "1");

    MainWindow window;
    window.show();
    auto *table = window.findChild<QTableView *>();
    QVERIFY(table);
    QTRY_VERIFY_WITH_TIMEOUT(table->model() && table->model()->rowCount() == 2, 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isScanning(), 8000);

    QComboBox *destination = nullptr;
    for (QComboBox *combo : window.findChildren<QComboBox *>()) {
        if (combo->findData(slm::canonicalOrAbsolute(destLibrary)) >= 0) {
            destination = combo;
            break;
        }
    }
    QVERIFY(destination);
    destination->setCurrentIndex(destination->findData(slm::canonicalOrAbsolute(destLibrary)));
    table->selectAll();
    QVERIFY(table->selectionModel()->selectedRows().size() == 2);
    QVERIFY(QMetaObject::invokeMethod(&window, "reviewMove", Qt::DirectConnection));

    bool reviewAccepted = false;
    bool reviewHasScroll = false;
    bool reviewToggled = false;
    bool reviewSelectionWorks = false;
    bool summaryClosed = false;
    QTimer watcher;
    watcher.setInterval(25);
    QObject::connect(&watcher, &QTimer::timeout, [&]() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog || !dialog->isVisible())
                continue;
            if (!reviewAccepted && dialog->windowTitle() == QStringLiteral("Review transfer")) {
                reviewHasScroll = dialog->findChild<QScrollArea *>() != nullptr;
                auto *start = dialog->findChild<QPushButton *>(QStringLiteral("startTransfer"));
                if (start && !reviewToggled) {
                    auto *reviewTable = dialog->findChild<QTableWidget *>();
                    if (reviewTable && reviewTable->rowCount() >= 2) {
                        auto *check =
                            qobject_cast<QCheckBox *>(reviewTable->cellWidget(0, 0));
                        if (check) {
                            check->setChecked(false);
                            QCoreApplication::processEvents();
                            for (QLabel *label : dialog->findChildren<QLabel *>()) {
                                if (label->text().contains(QStringLiteral("Move 1 game")))
                                    reviewSelectionWorks = true;
                            }
                            reviewSelectionWorks = reviewSelectionWorks && start->isEnabled();
                            check->setChecked(true);
                            QCoreApplication::processEvents();
                            reviewToggled = true;
                        }
                    }
                }
                if (start && start->isEnabled()) {
                    start->click();
                    reviewAccepted = true;

                }
            }
            if (auto *message = qobject_cast<QMessageBox *>(dialog);
                message && message != nullptr && !summaryClosed) {
                if (QAbstractButton *close = message->button(QMessageBox::Close))
                    close->click();
                else
                    message->accept();
                summaryClosed = true;
            }
        }
    });
    watcher.start();
    QTRY_VERIFY_WITH_TIMEOUT(reviewAccepted, 10000);
    QVERIFY(reviewHasScroll);
    QVERIFY(reviewToggled);
    QVERIFY(reviewSelectionWorks);
    QTRY_VERIFY_WITH_TIMEOUT(summaryClosed, 15000);
    watcher.stop();

    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/appmanifest_12346.acf")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/common/Test Game/game.dat")));
    QVERIFY(QFileInfo::exists(destLibrary + QStringLiteral("/steamapps/common/Test Game 2/game.dat")));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12345.acf")));
    QVERIFY(!QFileInfo::exists(sourceLibrary + QStringLiteral("/steamapps/appmanifest_12346.acf")));
    window.close();
}

QTEST_MAIN(GuiTest)
#include "tst_gui.moc"
