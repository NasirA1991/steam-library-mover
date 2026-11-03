// SPDX-License-Identifier: GPL-3.0-or-later
#include "librarydelegate.h"

#include "uiformat.h"

#include <QApplication>
#include <QFont>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QStyle>
#include <QStyledItemDelegate>

LibraryItemDelegate::LibraryItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize LibraryItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(0, 70);
}

void LibraryItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QString name = opt.text;
    opt.text.clear();
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    opt.icon = QIcon();
    QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const bool selected = option.state & QStyle::State_Selected;
    const QColor primary = selected ? option.palette.color(QPalette::HighlightedText)
                                    : option.palette.color(QPalette::Text);
    const QColor secondary = selected
        ? option.palette.color(QPalette::HighlightedText)
        : option.palette.color(QPalette::Disabled, QPalette::Text);

    const QRect content = option.rect.adjusted(9, 5, -9, -6);
    int textLeft = content.left();
    const QColor typeColor = index.data(ui::TypeColorRole).value<QColor>();
    if (typeColor.isValid()) {
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(typeColor);
        painter->drawRoundedRect(QRectF(content.left(), content.top() + 5, 4.0,
                                        content.height() - 10.0),
                                 2.0, 2.0);
        painter->restore();
        textLeft += 12;
    }
    if (!icon.isNull()) {
        const QSize iconSize(24, 24);
        const QRect iconRect(content.left(), content.top() + 3, iconSize.width(), iconSize.height());
        icon.paint(painter, iconRect, Qt::AlignCenter,
                   selected ? QIcon::Selected : QIcon::Normal, QIcon::Off);
        textLeft += iconSize.width() + 9;
    }

    QFont nameFont = option.font;
    nameFont.setBold(true);
    QFont subtitleFont = option.font;
    subtitleFont.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1.2));

    const QString subtitle = index.data(ui::SubtitleRole).toString();
    const QRect nameRect(textLeft, content.top(), content.right() - textLeft, 22);
    const QRect subtitleRect(textLeft, content.top() + 22, content.right() - textLeft, 18);
    painter->save();
    painter->setFont(nameFont);
    painter->setPen(primary);
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameRect.width()));
    painter->setFont(subtitleFont);
    painter->setPen(secondary);
    painter->drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                      QFontMetrics(subtitleFont).elidedText(subtitle, Qt::ElideRight,
                                                           subtitleRect.width()));

    const int usage = index.data(ui::UsagePercentRole).toInt();
    if (usage >= 0) {
        const QRect barRect(textLeft, content.bottom() - 7, content.right() - textLeft, 5);
        QColor track = option.palette.color(QPalette::Mid);
        track.setAlpha(80);
        painter->setPen(Qt::NoPen);
        painter->setBrush(track);
        painter->drawRoundedRect(barRect, 2.5, 2.5);
        QColor fill = QColor(QStringLiteral("#2e8b57"));
        if (usage >= 95)
            fill = QColor(QStringLiteral("#c0392b"));
        else if (usage >= 80)
            fill = QColor(QStringLiteral("#c58b16"));
        QRect valueRect = barRect;
        valueRect.setWidth(qBound(1, (barRect.width() * usage) / 100, barRect.width()));
        painter->setBrush(fill);
        painter->drawRoundedRect(valueRect, 2.5, 2.5);
    }
    painter->restore();
}
