/*
 * SPDX-FileCopyrightText: 2022 Paul A McAuley <kde@paulmcauley.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "stylesystemicontheme.h"
#include <QGraphicsColorizeEffect>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QIcon>
#include <memory>

namespace Breeze
{

void RenderStyleSystemIconTheme::paintIconFromSystemTheme(QString iconName)
{
    // QIcon::setThemeName(QIcon::themeName()); //doing this hack allows Adwaita icon theme to be partially loaded
    std::unique_ptr<QIcon> icon(new QIcon(QIcon::fromTheme(iconName)));
    QRect rect(QPoint(0, 0), QSize(m_iconWidth, m_iconWidth));

    if (m_internalSettings->colorizeSystemIcons()) {
        std::unique_ptr<QGraphicsScene> scene(new QGraphicsScene);
        QGraphicsPixmapItem *item = new QGraphicsPixmapItem; // raw pointer as QGraphicsScene takes ownership of QGraphicsPixmapItem

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        /* the following paragraph is a silly workaround to fix a Qt problem with multiple monitors with different DPIs on Wayland
         * When returning a pixmap from a QIcon Qt will give the pixmap the devicePixelRatio of the monitor with the highest devicePixelRatio
         * Qt does not give it the devicePixelRatio of the current monitor. This causes blurry icons on the lower-dpr screens.
         * Therefore have to make an icon scaled by the difference and set the devicePixelRatio manually
         * Qt6 should offer a better solution as has the option to specify the devicePixelRatio when requesting a QPixmap from a QIcon
         */
        QPixmap *iconPixmapToRender = nullptr;
        std::unique_ptr<QPixmap> iconPixmap(new QPixmap(icon->pixmap(QSize(m_iconWidth, m_iconWidth))));
        std::unique_ptr<QPixmap> iconPixmap2;
        qreal qIconDefaultDevicePixelRatio = iconPixmap->devicePixelRatioF();
        if (qAbs(qIconDefaultDevicePixelRatio - m_devicePixelRatio) < 0.05)
            iconPixmapToRender = iconPixmap.get();
        else {
            iconPixmap2.reset(new QPixmap());
            int reducedIconWidth = qRound(m_iconWidth * m_devicePixelRatio / qIconDefaultDevicePixelRatio);
            *iconPixmap2 = icon->pixmap(reducedIconWidth, reducedIconWidth);
            iconPixmap2->setDevicePixelRatio(m_devicePixelRatio);
            iconPixmapToRender = iconPixmap2.get();
        }
        if (iconPixmapToRender)
            item->setPixmap(*iconPixmapToRender);

#else
        item->setPixmap(icon->pixmap(QSize(m_iconWidth, m_iconWidth), m_devicePixelRatio)); // need Qt6 for this more straightforward line to work
#endif
        /* Tint the icon with the pen colour */
        QGraphicsColorizeEffect *effect = new QGraphicsColorizeEffect; // raw pointer as QGraphicsPixmapItem takes ownership of QGraphicsColorizeEffect
        effect->setColor(pen.color());
        item->setGraphicsEffect(effect);

        scene->addItem(item);
        scene->render(painter, rect, rect);
    } else
        icon->paint(painter, QRect(QPoint(0, 0), QSize(m_iconWidth, m_iconWidth)));
}

void RenderStyleSystemIconTheme::renderCloseIcon()
{
    paintIconFromSystemTheme(QStringLiteral("window-close-symbolic"));
}

void RenderStyleSystemIconTheme::renderMaximizeIcon()
{
    paintIconFromSystemTheme(QStringLiteral("window-maximize-symbolic"));
}

void RenderStyleSystemIconTheme::renderRestoreIcon()
{
    paintIconFromSystemTheme(QStringLiteral("window-restore-symbolic"));
}

void RenderStyleSystemIconTheme::renderMinimizeIcon()
{
    paintIconFromSystemTheme(QStringLiteral("window-minimize-symbolic"));
}

void RenderStyleSystemIconTheme::renderKeepBehindIcon()
{
    paintIconFromSystemTheme(QStringLiteral("window-keep-below-symbolic"));
}

void RenderStyleSystemIconTheme::renderKeepInFrontIcon()
{
    paintIconFromSystemTheme(QStringLiteral("window-keep-above-symbolic"));
}

void RenderStyleSystemIconTheme::renderContextHelpIcon()
{
    if ((!notInTitlebar) && boldButtonIcons) {
        // thicker pen in titlebar
        pen.setWidthF(pen.widthF() * 1.6);
    }

    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);

    // main body of question mark
    QPainterPath path;
    path.moveTo(7, 5);
    path.arcTo(QRectF(6.5, 3.5, 5.5, 5), 150, -160);
    path.cubicTo(QPointF(12, 9.5), QPointF(9, 7.5), QPointF(9, 11.5));
    painter->drawPath(path);

    // dot of question mark
    painter->setPen(Qt::NoPen);
    painter->setBrush(pen.color());
    if ((!notInTitlebar) && boldButtonIcons)
        painter->drawEllipse(QRectF(8, 14, 2, 2));
    else
        painter->drawEllipse(QRectF(8.25, 14.25, 1.5, 1.5));
}

void RenderStyleSystemIconTheme::renderShadeIcon()
{
    if ((!notInTitlebar) && boldButtonIcons) {
        // thicker pen in titlebar
        pen.setWidthF(pen.widthF() * 1.3);
        painter->setPen(pen);
    }
    RenderDecorationButtonIcon18By18::renderShadeIcon();
}

void RenderStyleSystemIconTheme::renderUnShadeIcon()
{
    if ((!notInTitlebar) && boldButtonIcons) {
        // thicker pen in titlebar
        pen.setWidthF(pen.widthF() * 1.3);
        painter->setPen(pen);
    }
    RenderDecorationButtonIcon18By18::renderUnShadeIcon();
}

}
