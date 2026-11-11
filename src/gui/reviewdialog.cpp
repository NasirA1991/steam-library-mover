// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/reviewdialog.h"

#include "core/mountinfo.h"
#include "core/planner.h"
#include "gui/theme.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QSize>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <algorithm>

ReviewDialog::ReviewDialog(const QVector<slm::MovePlan> &plans, const QString &destinationName,
                           quint64 destinationFree, quint64 destinationTotal,
                           const QHash<QString, QString> &sourceNames,
                           const slm::MoveOptions &options, QWidget *parent)
    : QDialog(parent), m_plans(plans), m_destinationName(destinationName),
      m_destinationFree(destinationFree), m_destinationTotal(destinationTotal),
      m_sourceNames(sourceNames), m_options(options), m_summary(buildSummary(plans))
{
    buildUi();
    updateReview();
}

ReviewDialog::Summary ReviewDialog::buildSummary(const QVector<slm::MovePlan> &plans) const
{
    Summary result;
    result.gameCount = plans.size();
    QSet<QString> issueSeen;
    QSet<QString> blockerSeen;
    for (const slm::MovePlan &plan : plans) {
        result.ntfsWarning = result.ntfsWarning || slm::hasWindowsFilesystemWarning(plan);
        result.totalBytes += plan.totalBytes;
        result.payloadBytes += plan.game.sizeOnDisk;
        const QString sourceName =
            m_sourceNames.value(plan.game.libraryPath, plan.game.libraryPath);
        if (!result.sources.contains(sourceName))
            result.sources.append(sourceName);
        for (const slm::Issue &issue : plan.issues) {
            const QString key = QStringLiteral("%1|%2|%3")
                                    .arg(static_cast<int>(issue.severity))
                                    .arg(issue.code, issue.detail);
            if (issueSeen.contains(key))
                continue;
            issueSeen.insert(key);
            if (issue.isBlocker() && issue.code != QStringLiteral("windows-filesystem-ack")) {
                result.anyBlocker = true;
                if (!blockerSeen.contains(key)) {
                    blockerSeen.insert(key);
                    result.blockers.append(issue);
                }
            }
            result.issueText << QStringLiteral("%1 — %2: %3%4")
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
    std::stable_sort(result.blockers.begin(), result.blockers.end(),
                     [&](const slm::Issue &a, const slm::Issue &b) {
                         return blockerRank(a) < blockerRank(b);
                     });
    bool hasCaseCollision = false;
    bool hasReadOnly = false;
    for (const slm::Issue &issue : result.blockers) {
        if (issue.code.startsWith(QStringLiteral("ntfs-case-collision")))
            hasCaseCollision = true;
        if (issue.code == QStringLiteral("source-readonly")
            || issue.code == QStringLiteral("destination-readonly")
            || issue.code == QStringLiteral("source-not-writable")
            || issue.code == QStringLiteral("destination-not-writable")) {
            hasReadOnly = true;
        }
    }
    if (hasCaseCollision) {
        result.guidance << QStringLiteral(
            "NTFS cannot store two names that differ only by case. Move to a case-sensitive "
            "Linux filesystem (Btrfs/ext4/XFS), or back up and resolve the conflicting source "
            "entry before retrying.");
    }
    if (hasReadOnly) {
        result.guidance << QStringLiteral(
            "A source or destination library is mounted read-only. Remount it read-write with "
            "the correct uid/gid before moving.");
    }
    if (result.guidance.isEmpty() && result.anyBlocker) {
        result.guidance
            << QStringLiteral("Review the blocker details below. No files have been changed.");
    }
    return result;
}

QVector<slm::MovePlan> ReviewDialog::selectedPlans() const
{
    QVector<slm::MovePlan> plans;
    for (int i = 0; i < m_plans.size(); ++i) {
        if (m_rowChecks.at(i)->isChecked())
            plans.append(m_plans.at(i));
    }
    return plans;
}

void ReviewDialog::updateEnabled(const Summary &summary)
{
    const bool acknowledged = !summary.ntfsWarning || m_ack->isChecked();
    m_okButton->setEnabled(!summary.anyBlocker && summary.gameCount > 0 && acknowledged);
    if (summary.gameCount == 0) {
        m_startHint->setText(QStringLiteral("Start is disabled: select at least one game."));
    } else if (summary.anyBlocker) {
        m_startHint->setText(QStringLiteral("Start is disabled: resolve the blocker above."));
    } else if (summary.ntfsWarning && !m_ack->isChecked()) {
        m_startHint->setText(
            QStringLiteral("Start is disabled: tick the NTFS acknowledgement below."));
    } else {
        m_startHint->clear();
    }
    m_startHint->setVisible(!m_startHint->text().isEmpty());
}

void ReviewDialog::updateReview()
{
    m_summary = buildSummary(selectedPlans());
    const QString muted =
        ui::isDarkTheme() ? QStringLiteral("#aab4c0") : QStringLiteral("#66717d");

    m_heading->setText(
        QStringLiteral("<b style='font-size:16px'>Review transfer</b><br>"
                       "<span style='color:%1'>Move %2 game%3 · %4 payload</span>")
            .arg(muted)
            .arg(m_summary.gameCount)
            .arg(m_summary.gameCount == 1 ? QString() : QStringLiteral("s"))
            .arg(m_summary.payloadBytes == 0 ? QStringLiteral("unknown size")
                                             : slm::formatBytes(m_summary.payloadBytes)));

    m_blocked->setVisible(m_summary.anyBlocker);
    if (m_summary.anyBlocker) {
        m_blockedTitle->setText(
            QStringLiteral("<b>Move blocked by %1 blocker%2</b>")
                .arg(m_summary.blockers.size())
                .arg(m_summary.blockers.size() == 1 ? QString() : QStringLiteral("s")));
        QStringList lines;
        const int shown = qMin(3, m_summary.blockers.size());
        for (int i = 0; i < shown; ++i) {
            const slm::Issue &issue = m_summary.blockers.at(i);
            QString line = QStringLiteral("• <b>%1</b>").arg(issue.message.toHtmlEscaped());
            if (!issue.detail.isEmpty())
                line += QStringLiteral("<br><span style='color:%1'>%2</span>")
                            .arg(muted, issue.detail.toHtmlEscaped());
            lines << line;
        }
        if (m_summary.blockers.size() > shown) {
            lines << QStringLiteral("• …and %1 more (see findings below)")
                          .arg(m_summary.blockers.size() - shown);
        }
        m_blockedReasons->setText(lines.join(QStringLiteral("<br>")));
        m_blockedHelp->setText(m_summary.guidance.join(QLatin1Char(' ')));
        m_blockedHelp->setVisible(!m_summary.guidance.isEmpty());
    }

    m_ntfsNotice->setVisible(m_summary.ntfsWarning);
    m_ack->setVisible(m_summary.ntfsWarning);
    m_why->setVisible(m_summary.anyBlocker);
    m_issues->setVisible(!m_summary.issueText.isEmpty());
    m_issues->setPlainText(m_summary.issueText.join(QLatin1Char('\n')));
    m_sourceKicker->setText(
        QStringLiteral("FROM · %1 LIBRAR%2")
            .arg(m_summary.sources.size())
            .arg(m_summary.sources.size() == 1 ? QStringLiteral("Y") : QStringLiteral("IES")));
    m_sourceTitle->setText(m_summary.sources.join(QStringLiteral(" + ")));
    m_destinationMeta->setText(
        QStringLiteral("%1 free of %2 · ~%3 after")
            .arg(slm::formatBytes(m_destinationFree), slm::formatBytes(m_destinationTotal),
                 slm::formatBytes(m_destinationFree > m_summary.totalBytes
                                      ? m_destinationFree - m_summary.totalBytes
                                      : 0)));
    m_total->setText(QStringLiteral("<span style='color:%1'>Total payload</span> &nbsp; "
                                   "<b>%2</b>")
                        .arg(muted, m_summary.payloadBytes == 0
                                        ? QStringLiteral("unknown")
                                        : slm::formatBytes(m_summary.payloadBytes)));
    m_spaceNeeded->setText(slm::formatBytes(m_summary.totalBytes));
    m_spaceNeededNote->setText(QStringLiteral("payload + prefix/shadercache"));
    const quint64 after = m_destinationFree > m_summary.totalBytes
        ? m_destinationFree - m_summary.totalBytes
        : 0;
    m_spaceAfter->setText(QStringLiteral("~%1 free").arg(slm::formatBytes(after)));
    m_spaceAfterNote->setText(
        QStringLiteral("source recovers ~%1").arg(slm::formatBytes(m_summary.totalBytes)));
    updateEnabled(m_summary);
}

QVector<slm::MovePlan> ReviewDialog::acceptedPlans() const
{
    QVector<slm::MovePlan> plans = selectedPlans();
    const bool acknowledged = m_summary.ntfsWarning && m_ack->isChecked();
    for (slm::MovePlan &plan : plans) {
        plan.options.fullVerify = m_fullVerify->isChecked();
        plan.options.allowNtfsNative = acknowledged;
        plan.options.keepSourceBackup = m_keepBackup->isChecked();
        if (acknowledged) {
            for (int i = plan.issues.size() - 1; i >= 0; --i) {
                if (plan.issues.at(i).code == QStringLiteral("windows-filesystem-ack"))
                    plan.issues.removeAt(i);
            }
        }
    }
    return plans;
}

void ReviewDialog::buildUi()
{
    setWindowTitle(QStringLiteral("Review transfer"));

    const QString muted =
        ui::isDarkTheme() ? QStringLiteral("#aab4c0") : QStringLiteral("#66717d");
    const QString border =
        ui::isDarkTheme() ? QStringLiteral("#3d444d") : QStringLiteral("#c4cad2");
    const QString danger =
        ui::isDarkTheme() ? QStringLiteral("#ff9b93") : QStringLiteral("#a11b1b");
    const QString dangerBg =
        ui::isDarkTheme() ? QStringLiteral("#3a2222") : QStringLiteral("#fdecec");

    const QRect available = screen() ? screen()->availableGeometry() : QRect(0, 0, 1280, 800);
    const int width = qMin(940, qMax(620, available.width() - 60));
    const int height = qMin(700, qMax(430, available.height() - 100));
    setMinimumSize(560, 400);
    resize(width, height);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 12);
    root->setSpacing(8);
    root->setSizeConstraint(QLayout::SetNoConstraint);

    m_heading = new QLabel(this);
    m_heading->setTextFormat(Qt::RichText);
    root->addWidget(m_heading);

    auto *scroll = new QScrollArea(this);
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

    m_blocked = new QFrame(body);
    m_blocked->setObjectName(QStringLiteral("blockedBanner"));
    m_blocked->setStyleSheet(
        QStringLiteral("QFrame#blockedBanner { background: %1; border: 1px solid %2; "
                       "border-radius: 5px; }")
            .arg(dangerBg, danger));
    auto *blockedLayout = new QVBoxLayout(m_blocked);
    blockedLayout->setContentsMargins(12, 8, 12, 8);
    blockedLayout->setSpacing(3);
    m_blockedTitle = new QLabel(m_blocked);
    m_blockedTitle->setTextFormat(Qt::RichText);
    m_blockedTitle->setStyleSheet(QStringLiteral("color: %1;").arg(danger));
    blockedLayout->addWidget(m_blockedTitle);
    m_blockedReasons = new QLabel(m_blocked);
    m_blockedReasons->setTextFormat(Qt::RichText);
    m_blockedReasons->setWordWrap(true);
    m_blockedReasons->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_blockedReasons->setStyleSheet(QStringLiteral("color: %1;").arg(muted));
    blockedLayout->addWidget(m_blockedReasons);
    m_blockedHelp = new QLabel(m_blocked);
    m_blockedHelp->setWordWrap(true);
    m_blockedHelp->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    blockedLayout->addWidget(m_blockedHelp);
    layout->addWidget(m_blocked);

    m_ntfsNotice = new QLabel(
        QStringLiteral("NTFS destination: tick the Windows-filesystem acknowledgement at "
                       "the bottom of this review before Start can be enabled."),
        body);
    m_ntfsNotice->setWordWrap(true);
    m_ntfsNotice->setStyleSheet(
        QStringLiteral("color: %1; background: %2; border: 1px solid %3; "
                       "border-radius: 4px; padding: 6px 8px; font-size: 10px;")
            .arg(ui::isDarkTheme() ? QStringLiteral("#f2c96d") : QStringLiteral("#7a5200"),
                 ui::isDarkTheme() ? QStringLiteral("#332b1b") : QStringLiteral("#fff7e0"),
                 ui::isDarkTheme() ? QStringLiteral("#6b5625") : QStringLiteral("#e3c26a")));
    layout->addWidget(m_ntfsNotice);

    const auto makeCard = [&](const QString &kicker, const QString &title, const QString &meta,
                              QLabel **kickerOut, QLabel **titleOut, QLabel **metaOut) {
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
        metaLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
        metaLabel->setWordWrap(true);
        cardLayout->addWidget(kickerLabel);
        cardLayout->addWidget(titleLabel);
        cardLayout->addWidget(metaLabel);
        if (kickerOut)
            *kickerOut = kickerLabel;
        if (titleOut)
            *titleOut = titleLabel;
        if (metaOut)
            *metaOut = metaLabel;
        return card;
    };

    auto *route = new QHBoxLayout();
    route->setSpacing(8);
    route->addWidget(makeCard(QStringLiteral("FROM · %1 LIBRAR%2")
                                  .arg(m_summary.sources.size())
                                  .arg(m_summary.sources.size() == 1 ? QStringLiteral("Y")
                                                                   : QStringLiteral("IES")),
                              m_summary.sources.join(QStringLiteral(" + ")),
                              QStringLiteral("Verified source libraries"), &m_sourceKicker,
                              &m_sourceTitle, nullptr),
                     1);
    auto *arrow = new QLabel(QStringLiteral("→"), body);
    arrow->setAlignment(Qt::AlignCenter);
    arrow->setStyleSheet(QStringLiteral("font-size: 18px; color: %1;").arg(muted));
    route->addWidget(arrow);
    route->addWidget(makeCard(QStringLiteral("TO"), m_destinationName,
                              QStringLiteral("%1 free of %2 · ~%3 after")
                                  .arg(slm::formatBytes(m_destinationFree),
                                       slm::formatBytes(m_destinationTotal),
                                       slm::formatBytes(m_destinationFree)),
                              nullptr, nullptr, &m_destinationMeta),
                     1);
    layout->addLayout(route);

    m_table = new QTableWidget(body);
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Move"), QStringLiteral("Game"), QStringLiteral("Payload")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(32);
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->setRowCount(m_plans.size());
    m_table->setIconSize(QSize(22, 30));
    for (int i = 0; i < m_plans.size(); ++i) {
        const slm::Game &game = m_plans.at(i).game;
        auto *check = new QCheckBox(m_table);
        check->setChecked(true);
        check->setToolTip(QStringLiteral("Include %1 in this transfer").arg(game.name));
        m_table->setCellWidget(i, 0, check);
        m_rowChecks.append(check);
        auto *nameItem = new QTableWidgetItem(game.name);
        if (!game.artworkPath.isEmpty()) {
            const QPixmap pixmap(game.artworkPath);
            if (!pixmap.isNull())
                nameItem->setIcon(QIcon(pixmap));
        }
        m_table->setItem(i, 1, nameItem);
        m_table->setItem(i, 2, new QTableWidgetItem(
            game.sizeOnDisk == 0 ? QStringLiteral("unknown") : slm::formatBytes(game.sizeOnDisk)));
    }
    const int tableHeader = m_table->horizontalHeader()->sizeHint().height();
    const int visibleRows = qMin(m_plans.size(), 5);
    m_table->setFixedHeight(tableHeader + visibleRows * 32 + 4);
    layout->addWidget(m_table);

    m_total = new QLabel(body);
    m_total->setTextFormat(Qt::RichText);
    layout->addWidget(m_total);

    auto *spaceBox = new QFrame(body);
    spaceBox->setObjectName(QStringLiteral("spaceBox"));
    spaceBox->setStyleSheet(
        QStringLiteral("QFrame#spaceBox { background: palette(base); border: 1px solid %1; "
                       "border-radius: 5px; }")
            .arg(border));
    auto *spaceLayout = new QHBoxLayout(spaceBox);
    spaceLayout->setContentsMargins(12, 8, 12, 8);
    const auto addSpace = [&](const QString &label, const QString &value, const QString &note,
                              QLabel **valueOut, QLabel **noteOut) {
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
        if (valueOut)
            *valueOut = val;
        if (noteOut)
            *noteOut = fine;
    };
    addSpace(QStringLiteral("DESTINATION NOW"), slm::formatBytes(m_destinationFree),
             m_destinationName, nullptr, nullptr);
    addSpace(QStringLiteral("NEEDED DURING COPY"), slm::formatBytes(m_summary.totalBytes),
             QStringLiteral("payload + prefix/shadercache"), &m_spaceNeeded,
             &m_spaceNeededNote);
    addSpace(QStringLiteral("AFTER CLEANUP"),
             QStringLiteral("~%1 free").arg(slm::formatBytes(m_destinationFree)),
             QStringLiteral("source recovers ~%1").arg(slm::formatBytes(m_summary.totalBytes)),
             &m_spaceAfter, &m_spaceAfterNote);
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

    m_issues = new QTextBrowser(body);
    m_issues->setMaximumHeight(92);
    m_issues->setStyleSheet(
        QStringLiteral("QTextBrowser { background: palette(base); border: 1px solid %1; "
                       "border-radius: 4px; }")
            .arg(border));
    layout->addWidget(m_issues);

    m_fullVerify = new QCheckBox(
        QStringLiteral("Full SHA-256 verification (default; uncheck for quick mode)"), body);
    m_fullVerify->setChecked(m_options.fullVerify);
    layout->addWidget(m_fullVerify);
    auto *verifyNote = new QLabel(
        QStringLiteral("Full verification is optional and does not gate Start; it only controls how "
                       "deeply copied files are checked."),
        body);
    verifyNote->setWordWrap(true);
    verifyNote->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    layout->addWidget(verifyNote);
    auto *quickVerifyRisk = new QLabel(
        QStringLiteral("Warning: quick verification checks paths and sizes only and cannot detect "
                       "same-size corruption."),
        body);
    quickVerifyRisk->setWordWrap(true);
    quickVerifyRisk->setVisible(!m_fullVerify->isChecked());
    QPalette riskPalette = quickVerifyRisk->palette();
    riskPalette.setColor(QPalette::WindowText, QColor(QStringLiteral("#d9534f")));
    quickVerifyRisk->setPalette(riskPalette);
    layout->addWidget(quickVerifyRisk);
    connect(m_fullVerify, &QCheckBox::toggled, this,
            [quickVerifyRisk](bool checked) { quickVerifyRisk->setVisible(!checked); });

    m_keepBackup = new QCheckBox(
        QStringLiteral("Keep a source backup (hidden .slm-backup-* folders)"), body);
    m_keepBackup->setChecked(m_options.keepSourceBackup);
    layout->addWidget(m_keepBackup);
    m_ack = new QCheckBox(
        QStringLiteral("I understand the destination is a Windows-compatible filesystem; "
                       "native Linux games may not run there. (Required for this destination.)"),
        body);
    layout->addWidget(m_ack);

    auto *footer = new QHBoxLayout();
    auto *reviewNote = new QLabel(QStringLiteral("Review changes nothing until you start."), this);
    reviewNote->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    footer->addWidget(reviewNote);
    m_startHint = new QLabel(this);
    m_startHint->setWordWrap(true);
    m_startHint->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(muted));
    m_startHint->setVisible(false);
    footer->addWidget(m_startHint);
    m_why = new QPushButton(QStringLiteral("Why blocked?"), this);
    m_why->setAutoDefault(false);
    m_why->setObjectName(QStringLiteral("whyBlocked"));
    footer->addWidget(m_why);
    footer->addStretch();
    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    auto *preflight = new QPushButton(QStringLiteral("Preflight only"), this);
    m_okButton = new QPushButton(QStringLiteral("Start transfer →"), this);
    m_okButton->setObjectName(QStringLiteral("startTransfer"));
    m_okButton->setDefault(true);
    cancelButton->setAutoDefault(false);
    preflight->setAutoDefault(false);
    footer->addWidget(cancelButton);
    footer->addWidget(preflight);
    footer->addWidget(m_okButton);
    root->addLayout(footer);

    connect(m_ack, &QCheckBox::toggled, this, [this](bool) { updateEnabled(m_summary); });
    for (QCheckBox *check : m_rowChecks) {
        connect(check, &QCheckBox::toggled, this, [this](bool) { updateReview(); });
    }
    connect(m_why, &QPushButton::clicked, this, [this]() {
        QString html = QStringLiteral("<b>The move cannot start</b><br><br>");
        for (const slm::Issue &issue : m_summary.blockers) {
            html += QStringLiteral("<b>%1</b><br>").arg(issue.message.toHtmlEscaped());
            if (!issue.detail.isEmpty())
                html += QStringLiteral("%1<br>").arg(issue.detail.toHtmlEscaped());
            html += QStringLiteral("<br>");
        }
        if (!m_summary.guidance.isEmpty()) {
            html += QStringLiteral("<b>What to do</b><br>");
            for (const QString &line : m_summary.guidance)
                html += QStringLiteral("• %1<br>").arg(line.toHtmlEscaped());
            html += QStringLiteral("<br>");
        }
        html += QStringLiteral("No files have been changed.");
        QMessageBox box(QMessageBox::Warning, QStringLiteral("Why is this blocked?"), html,
                        QMessageBox::Close, this);
        box.setTextFormat(Qt::RichText);
        box.exec();
    });
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_okButton, &QPushButton::clicked, this, [this]() {
        if (m_okButton->isEnabled())
            accept();
    });
    connect(preflight, &QPushButton::clicked, this, [this]() {
        const QVector<slm::MovePlan> plans = selectedPlans();
        QStringList what;
        what << QStringLiteral("%1 game(s), %2 total")
                    .arg(plans.size())
                    .arg(slm::formatBytes(m_summary.totalBytes));
        what << QStringLiteral("Destination: %1 (%2 free)")
                    .arg(m_destinationName, slm::formatBytes(m_destinationFree));
        const slm::MountTable mounts = slm::MountTable::load();
        QStringList changed;
        for (const slm::MovePlan &plan : plans) {
            QString checkError;
            if (!slm::MovePlanner::revalidate(plan, mounts, &checkError))
                changed << QStringLiteral("%1: %2").arg(plan.game.name, checkError);
        }
        if (changed.isEmpty()) {
            what << QStringLiteral("Fresh re-check passed: paths, manifests, Steam state and free "
                                   "space are unchanged.");
        } else {
            what << QStringLiteral("Fresh re-check found changes:");
            what += changed;
        }
        what << QStringLiteral("No files have been changed. This is a preflight-only check.");
        if (m_summary.anyBlocker)
            what.prepend(QStringLiteral("Preflight has blockers; Start is disabled."));
        QMessageBox::information(this, QStringLiteral("Preflight only"),
                                 what.join(QLatin1Char('\n')));
    });
}
