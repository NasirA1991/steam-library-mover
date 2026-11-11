// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/types.h"

#include <QDialog>
#include <QHash>
#include <QStringList>
#include <QVector>

class QCheckBox;
class QFrame;
class QLabel;
class QPushButton;
class QTableWidget;
class QTextBrowser;

class ReviewDialog : public QDialog
{
public:
    ReviewDialog(const QVector<slm::MovePlan> &plans, const QString &destinationName,
                 quint64 destinationFree, quint64 destinationTotal,
                 const QHash<QString, QString> &sourceNames, const slm::MoveOptions &options,
                 QWidget *parent = nullptr);

    QVector<slm::MovePlan> acceptedPlans() const;

private:
    struct Summary
    {
        quint64 totalBytes = 0;
        quint64 payloadBytes = 0;
        int gameCount = 0;
        bool anyBlocker = false;
        bool ntfsWarning = false;
        QStringList sources;
        QStringList issueText;
        QVector<slm::Issue> blockers;
        QStringList guidance;
    };

    void buildUi();
    Summary buildSummary(const QVector<slm::MovePlan> &plans) const;
    QVector<slm::MovePlan> selectedPlans() const;
    void updateEnabled(const Summary &summary);
    void updateReview();

    QVector<slm::MovePlan> m_plans;
    QString m_destinationName;
    quint64 m_destinationFree = 0;
    quint64 m_destinationTotal = 0;
    QHash<QString, QString> m_sourceNames;
    slm::MoveOptions m_options;
    Summary m_summary;

    QVector<QCheckBox *> m_rowChecks;
    QLabel *m_heading = nullptr;
    QLabel *m_blockedTitle = nullptr;
    QLabel *m_blockedReasons = nullptr;
    QLabel *m_blockedHelp = nullptr;
    QLabel *m_ntfsNotice = nullptr;
    QLabel *m_sourceKicker = nullptr;
    QLabel *m_sourceTitle = nullptr;
    QLabel *m_destinationMeta = nullptr;
    QLabel *m_total = nullptr;
    QLabel *m_spaceNeeded = nullptr;
    QLabel *m_spaceNeededNote = nullptr;
    QLabel *m_spaceAfter = nullptr;
    QLabel *m_spaceAfterNote = nullptr;
    QLabel *m_startHint = nullptr;
    QFrame *m_blocked = nullptr;
    QTableWidget *m_table = nullptr;
    QTextBrowser *m_issues = nullptr;
    QCheckBox *m_fullVerify = nullptr;
    QCheckBox *m_keepBackup = nullptr;
    QCheckBox *m_ack = nullptr;
    QPushButton *m_why = nullptr;
    QPushButton *m_okButton = nullptr;
};
