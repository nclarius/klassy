/*
* SPDX-FileCopyrightText: 2014 Martin Gräßlin <mgraesslin@kde.org>
* SPDX-FileCopyrightText: 2014 Hugo Pereira Da Costa <hugo.pereira@free.fr>
* SPDX-FileCopyrightText: 2018 Vlad Zahorodnii <vlad.zahorodnii@kde.org>
* SPDX-FileCopyrightText: 2021 Paul A McAuley <kde@paulmcauley.com>
*
* SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/


#include "breezedecoration.h"

#if CLASSIK_DECORATION_DEBUG_MODE
#include "setqdebug_logging.h"
#endif

#include "breezesettingsprovider.h"
#include "config-breeze.h"
#include "config/breezeconfigwidget.h"

#include "breezebutton.h"
#include "breezesizegrip.h"

#include "breezeboxshadowrenderer.h"

#include <KDecoration2/DecorationButtonGroup>
#include <KDecoration2/DecorationShadow>

#include <KConfigGroup>
#include <KColorUtils>
#include <KSharedConfig>
#include <KPluginFactory>
#include <KWindowSystem>

#include <QPainter>
#include <QTextStream>
#include <QTimer>
#include <QDBusConnection>

#if BREEZE_HAVE_X11
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <private/qtx11extras_p.h>
#else
#include <QX11Info>
#endif
#endif

#include <cmath>

K_PLUGIN_FACTORY_WITH_JSON(
    BreezeDecoFactory,
    "breeze.json",
    registerPlugin<Breeze::Decoration>();
    registerPlugin<Breeze::Button>();
    registerPlugin<Breeze::ConfigWidget>();
)

namespace
{
    struct ShadowParams {
        ShadowParams()
            : offset(QPoint(0, 0))
            , radius(0)
            , opacity(0) {}

        ShadowParams(const QPoint &offset, int radius, qreal opacity)
            : offset(offset)
            , radius(radius)
            , opacity(opacity) {}

        QPoint offset;
        int radius;
        qreal opacity;
    };

    struct CompositeShadowParams {
        CompositeShadowParams() = default;

        CompositeShadowParams(
                const QPoint &offset,
                const ShadowParams &shadow1,
                const ShadowParams &shadow2)
            : offset(offset)
            , shadow1(shadow1)
            , shadow2(shadow2) {}

        bool isNone() const {
            return qMax(shadow1.radius, shadow2.radius) == 0;
        }

        QPoint offset;
        ShadowParams shadow1;
        ShadowParams shadow2;
    };

    const CompositeShadowParams s_shadowParams[] = {
        // None
        CompositeShadowParams( //hacked in by PAM with small values except with opacity 0; this is to allow a thin window outline to be drawn without a shadow
            QPoint(0, 4),
            ShadowParams(QPoint(0, 0), 16, 0),
            ShadowParams(QPoint(0, -2), 8, 0)),
        // Small
        CompositeShadowParams(
            QPoint(0, 4),
            ShadowParams(QPoint(0, 0), 16, 1),
            ShadowParams(QPoint(0, -2), 8, 0.4)),
        // Medium
        CompositeShadowParams(
            QPoint(0, 8),
            ShadowParams(QPoint(0, 0), 32, 0.9),
            ShadowParams(QPoint(0, -4), 16, 0.3)),
        // Large
        CompositeShadowParams(
            QPoint(0, 12),
            ShadowParams(QPoint(0, 0), 48, 0.8),
            ShadowParams(QPoint(0, -6), 24, 0.2)),
        // Very large
        CompositeShadowParams(
            QPoint(0, 16),
            ShadowParams(QPoint(0, 0), 64, 0.7),
            ShadowParams(QPoint(0, -8), 32, 0.1)),
    };

    inline CompositeShadowParams lookupShadowParams(int size)
    {
        switch (size) {
        case Breeze::InternalSettings::ShadowNone:
            return s_shadowParams[0];
        case Breeze::InternalSettings::ShadowSmall:
            return s_shadowParams[1];
        case Breeze::InternalSettings::ShadowMedium:
            return s_shadowParams[2];
        case Breeze::InternalSettings::ShadowLarge:
            return s_shadowParams[3];
        case Breeze::InternalSettings::ShadowVeryLarge:
            return s_shadowParams[4];
        default:
            // Fallback to the Large size.
            return s_shadowParams[3];
        }
    }
}

namespace Breeze
{

    using KDecoration2::ColorRole;
    using KDecoration2::ColorGroup;

    //________________________________________________________________
    static int g_sDecoCount = 0;
    static int g_shadowSizeEnum = InternalSettings::ShadowLarge;
    static int g_shadowStrength = 255;
    static QColor g_shadowColor = Qt::black;
    static qreal g_cornerRadius = 3;
    static qreal g_systemScaleFactor = 1;
    static bool g_hasNoBorders = true;
    static int g_thinWindowOutlineStyle = 0;
    static QColor g_thinWindowOutlineCustomColor = Qt::black;
    static qreal g_thinWindowOutlineThickness = 1;
    static QSharedPointer<KDecoration2::DecorationShadow> g_sShadow;
    static QSharedPointer<KDecoration2::DecorationShadow> g_sShadowInactive;


    //________________________________________________________________
    Decoration::Decoration(QObject *parent, const QVariantList &args)
        : KDecoration2::Decoration(parent, args)
        , m_animation( new QVariantAnimation( this ) )
        , m_shadowAnimation( new QVariantAnimation( this ) )
    {
        g_sDecoCount++;
        
#if CLASSIK_DECORATION_DEBUG_MODE
        setDebugOutput(CLASSIK_QDEBUG_OUTPUT_PATH_RELATIVE_HOME);
#endif
    }

    //________________________________________________________________
    Decoration::~Decoration()
    {
        g_sDecoCount--;
        if (g_sDecoCount == 0) {
            // last deco destroyed, clean up shadow
            g_sShadow.clear();
        }

        deleteSizeGrip();

    }

    //________________________________________________________________
    void Decoration::setOpacity( qreal value )
    {
        if( m_opacity == value ) return;
        m_opacity = value;
        update();

        if( m_sizeGrip ) m_sizeGrip->update();
    }

    //________________________________________________________________
    QColor Decoration::titleBarColor(bool returnNonAnimatedColor) const
    {
        
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        if( hideTitleBar() && !m_internalSettings->useTitlebarColorForAllBorders() ) return c->color( ColorGroup::Inactive, ColorRole::TitleBar );
        
        QColor activeTitleBarColor = c->color( ColorGroup::Active, ColorRole::TitleBar );
        QColor inactiveTitlebarColor = c->color( ColorGroup::Inactive, ColorRole::TitleBar );
        if( m_internalSettings->opaqueMaximizedTitlebars() && c->isMaximized() ) activeTitleBarColor.setAlpha(255);
        
        //do not animate titlebar if there is a tools area/header area as it causes glitches
        if( !m_toolsAreaWillBeDrawn && m_animation->state() == QAbstractAnimation::Running && !returnNonAnimatedColor )
        {
            return KColorUtils::mix(
                inactiveTitlebarColor,
                activeTitleBarColor,
                m_opacity );
        } else return  c->isActive() ? activeTitleBarColor : inactiveTitlebarColor;

    }
    
    QColor Decoration::titleBarColorWithAddedTransparency() const
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        QColor color( titleBarColor() );
        color.setAlphaF( color.alphaF() * ( c->isActive() ? m_addedTitleBarOpacityActive : m_addedTitleBarOpacityInactive ) );
        return color;
    }

    //________________________________________________________________
    QColor Decoration::titleBarSeparatorColor() const
    {

        auto c = client().toStrongRef();
        Q_ASSERT(c);
        if( !m_internalSettings->drawTitleBarSeparator() ) return QColor();
        if( m_animation->state() == QAbstractAnimation::Running )
        {
            QColor color( m_systemAccentColors->highlight );
            color.setAlpha( color.alpha()*m_opacity );
            return color;
        } else if( c->isActive() ) return m_systemAccentColors->highlight;
        else return QColor();
    }
    
    QColor Decoration::accentedWindowOutlineColor(QColor customColor) const
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        QColor activeColor;
        QColor inactiveColor;
        
        if( customColor.isValid() ){
            activeColor = customColor;
            activeColor.setAlphaF( activeColor.alphaF() * 0.87 );
            inactiveColor = customColor;
            inactiveColor.setAlphaF( inactiveColor.alphaF() * 0.3 );
        } else {
            activeColor = m_systemAccentColors->highlight;
            activeColor.setAlphaF( activeColor.alphaF() * 0.87 );
            inactiveColor = m_systemAccentColors->highlightLessSaturated;
            inactiveColor.setAlphaF( inactiveColor.alphaF() * 0.4 );
        }
        
        if( m_animation->state() == QAbstractAnimation::Running )
        {
            return KColorUtils::mix( inactiveColor, activeColor, m_opacity );
        } else if( c->isActive() ) return activeColor;
        else return inactiveColor;
    }
    
    QColor Decoration::fontMixedAccentWindowOutlineColor(QColor customColor) const
    {

        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        QColor fontColorActive = c->color( ColorGroup::Active, ColorRole::Foreground );
        QColor fontColorInactive = c->color( ColorGroup::Inactive, ColorRole::Foreground );
        QColor accentColorActive;
        QColor accentColorInactive;
        
        if( customColor.isValid() ){
            accentColorActive = customColor;
            accentColorInactive = customColor;
        } else {
            accentColorActive = m_systemAccentColors->buttonFocus;
            accentColorInactive = m_systemAccentColors->buttonHover;
        }
        
        
        QColor activeColor = KColorUtils::mix( fontColorActive, accentColorActive, 0.75 );
        activeColor.setAlphaF( activeColor.alphaF() * 0.4 );
        
        QColor inactiveColor = KColorUtils::mix( fontColorInactive, accentColorInactive, 0.75 );
        inactiveColor.setAlphaF( inactiveColor.alphaF() * 0.25 );
        
        if( m_animation->state() == QAbstractAnimation::Running )
        {
            return KColorUtils::mix( inactiveColor, activeColor, m_opacity );
        } else if( c->isActive() ) return activeColor;
        else return inactiveColor;
    }

    //________________________________________________________________
    QColor Decoration::fontColor() const
    {

        auto c = client().toStrongRef();
        Q_ASSERT(c);
        if( m_animation->state() == QAbstractAnimation::Running )
        {
            return KColorUtils::mix(
                c->color( ColorGroup::Inactive, ColorRole::Foreground ),
                c->color( ColorGroup::Active, ColorRole::Foreground ),
                m_opacity );
        } else return  c->color( c->isActive() ? ColorGroup::Active : ColorGroup::Inactive, ColorRole::Foreground );

    }

    //________________________________________________________________
    void Decoration::init()
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        // active state change animation
        // It is important start and end value are of the same type, hence 0.0 and not just 0
        m_animation->setStartValue( 0.0 );
        m_animation->setEndValue( 1.0 );
        // Linear to have the same easing as Breeze animations
        m_animation->setEasingCurve( QEasingCurve::Linear );
        connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setOpacity(value.toReal());
        });

        m_shadowAnimation->setStartValue( 0.0 );
        m_shadowAnimation->setEndValue( 1.0 );
        m_shadowAnimation->setEasingCurve( QEasingCurve::OutCubic );
        connect(m_shadowAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            m_shadowOpacity = value.toReal();
            updateShadow();
        });

        // use DBus connection to update on breeze configuration change
        auto dbus = QDBusConnection::sessionBus();
        dbus.connect( QString(),
            QStringLiteral( "/KGlobalSettings" ),
            QStringLiteral( "org.kde.KGlobalSettings" ),
            QStringLiteral( "notifyChange" ), this, SLOT(reconfigure()) );

        reconfigure();
        updateTitleBar();
        auto s = settings();
        connect(s.data(), &KDecoration2::DecorationSettings::borderSizeChanged, this, &Decoration::recalculateBorders);
        connect(s.data(), &KDecoration2::DecorationSettings::borderSizeChanged, this, &Decoration::updateBlur); //for the case when a border with transparency

        // a change in font might cause the borders to change
        connect(s.data(), &KDecoration2::DecorationSettings::fontChanged, this, &Decoration::recalculateBorders);
        connect(s.data(), &KDecoration2::DecorationSettings::fontChanged, this, &Decoration::updateBlur); //for the case when a border with transparency
        connect(s.data(), &KDecoration2::DecorationSettings::spacingChanged, this, &Decoration::recalculateBorders);
        connect(s.data(), &KDecoration2::DecorationSettings::spacingChanged, this, &Decoration::updateBlur); //for the case when a border with transparency

        // buttons
        connect(s.data(), &KDecoration2::DecorationSettings::spacingChanged, this, &Decoration::updateButtonsGeometryDelayed);
        connect(s.data(), &KDecoration2::DecorationSettings::decorationButtonsLeftChanged, this, &Decoration::updateButtonsGeometryDelayed);
        connect(s.data(), &KDecoration2::DecorationSettings::decorationButtonsRightChanged, this, &Decoration::updateButtonsGeometryDelayed);

        // full reconfiguration
        connect(s.data(), &KDecoration2::DecorationSettings::reconfigured, this, &Decoration::reconfigure);
        connect(s.data(), &KDecoration2::DecorationSettings::reconfigured, SettingsProvider::self(), &SettingsProvider::reconfigure, Qt::UniqueConnection );
        connect(s.data(), &KDecoration2::DecorationSettings::reconfigured, this, &Decoration::updateButtonsGeometryDelayed);

        connect(c.data(), &KDecoration2::DecoratedClient::adjacentScreenEdgesChanged, this, &Decoration::recalculateBorders);
        connect(c.data(), &KDecoration2::DecoratedClient::maximizedHorizontallyChanged, this, &Decoration::recalculateBorders);
        connect(c.data(), &KDecoration2::DecoratedClient::maximizedVerticallyChanged, this, &Decoration::recalculateBorders);
        connect(c.data(), &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::recalculateBorders);
        connect(c.data(), &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::forceUpdateShadow);
        connect(c.data(), &KDecoration2::DecoratedClient::captionChanged, this,
            [this]()
            {
                // update the caption area
                update(titleBar());
            }
        );

        connect(c.data(), &KDecoration2::DecoratedClient::activeChanged, this, &Decoration::updateAnimationState);
        connect(c.data(), &KDecoration2::DecoratedClient::activeChanged, this, &Decoration::updateOpaque);
        connect(c.data(), &KDecoration2::DecoratedClient::activeChanged, this, &Decoration::updateBlur);
        connect(c.data(), &KDecoration2::DecoratedClient::widthChanged, this, &Decoration::updateTitleBar);
        connect(c.data(), &KDecoration2::DecoratedClient::sizeChanged, this, &Decoration::updateBlur);

        connect(c.data(), &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::setAddedTitleBarOpacity);
        connect(c.data(), &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateTitleBar);
        connect(c.data(), &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateOpaque);

        connect(c.data(), &KDecoration2::DecoratedClient::widthChanged, this, &Decoration::updateButtonsGeometry);
        connect(c.data(), &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateButtonsGeometry);
        connect(c.data(), &KDecoration2::DecoratedClient::adjacentScreenEdgesChanged, this, &Decoration::updateButtonsGeometry);
        connect(c.data(), &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::updateButtonsGeometry);
        
        connect(c.data(), &KDecoration2::DecoratedClient::paletteChanged, this, &Decoration::reconfigure);
        connect(c.data(), &KDecoration2::DecoratedClient::paletteChanged, this, &Decoration::forceUpdateShadow);

        createButtons();
        updateShadow();
    }

    //________________________________________________________________
    void Decoration::updateTitleBar()
    {
        auto s = settings();
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        //TODO: check whether isMaximized() should really be split into isMaximizedHorizontally() and isMaximizedVertically()
        const bool maximized = isMaximized();
        int width, height, x, y;
        setScaledTitleBarTopBottomMargins();
        setScaledTitleBarSideMargins();
        
        //prevents resize handles appearing in button at top window edge for large full-height buttons
        if( m_buttonSize == ButtonSize::FullHeight && !(m_internalSettings->drawBorderOnMaximizedWindows() && c->isMaximizedVertically()) )
        {
            width =  maximized ? c->width() : c->width() - 2*m_scaledTitleBarSideMargins;
            height = borderTop();
            x = maximized ? 0 : m_scaledTitleBarSideMargins;
            y = 0;
            
        } else 
        {   
            // for smaller circular buttons increase the resizable area
            width =  maximized ? c->width() : c->width() - 2*m_scaledTitleBarSideMargins;
            height = maximized ? borderTop() : borderTop() - m_scaledTitleBarTopMargin;
            x = maximized ? 0 : m_scaledTitleBarSideMargins;
            y = maximized ? 0 : m_scaledTitleBarTopMargin;
        }
        
        setTitleBar(QRect(x, y, width, height));
    }

    //________________________________________________________________
    void Decoration::updateAnimationState()
    {
        if( m_shadowAnimation->duration() > 0 )
        {

            auto c = client().toStrongRef();
            Q_ASSERT(c);
            m_shadowAnimation->setDirection( c->isActive() ? QAbstractAnimation::Forward : QAbstractAnimation::Backward );
            m_shadowAnimation->setEasingCurve( c->isActive() ? QEasingCurve::OutCubic : QEasingCurve::InCubic );
            if( m_shadowAnimation->state() != QAbstractAnimation::Running ) m_shadowAnimation->start();

        } else {

            updateShadow();

        }

        if( m_animation->duration() > 0 )
        {

            auto c = client().toStrongRef();
            Q_ASSERT(c);
            m_animation->setDirection( c->isActive() ? QAbstractAnimation::Forward : QAbstractAnimation::Backward );
            if( m_animation->state() != QAbstractAnimation::Running ) m_animation->start();

        } else {

            update();

        }
    }

    //________________________________________________________________
    void Decoration::updateSizeGripVisibility()
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        if( m_sizeGrip )
        { m_sizeGrip->setVisible( c->isResizeable() && !isMaximized() && !c->isShaded() ); }
    }

    //________________________________________________________________
    int Decoration::borderSize(bool bottom) const
    {
        const int baseSize = settings()->smallSpacing();
        if( m_internalSettings && (m_internalSettings->mask() & BorderSize ) )
        {
            switch (m_internalSettings->borderSize()) {
                case InternalSettings::BorderNone: return 0;
                case InternalSettings::BorderNoSides: return bottom ? qMax(4, baseSize) : 0;
                default:
                case InternalSettings::BorderTiny: return bottom ? qMax(4, baseSize) : baseSize;
                case InternalSettings::BorderNormal: return baseSize*2;
                case InternalSettings::BorderLarge: return baseSize*3;
                case InternalSettings::BorderVeryLarge: return baseSize*4;
                case InternalSettings::BorderHuge: return baseSize*5;
                case InternalSettings::BorderVeryHuge: return baseSize*6;
                case InternalSettings::BorderOversized: return baseSize*10;
            }

        } else {

            switch (settings()->borderSize()) {
                case KDecoration2::BorderSize::None: return 0;
                case KDecoration2::BorderSize::NoSides: return bottom ? qMax(4, baseSize) : 0;
                default:
                case KDecoration2::BorderSize::Tiny: return bottom ? qMax(4, baseSize) : baseSize;
                case KDecoration2::BorderSize::Normal: return baseSize*2;
                case KDecoration2::BorderSize::Large: return baseSize*3;
                case KDecoration2::BorderSize::VeryLarge: return baseSize*4;
                case KDecoration2::BorderSize::Huge: return baseSize*5;
                case KDecoration2::BorderSize::VeryHuge: return baseSize*6;
                case KDecoration2::BorderSize::Oversized: return baseSize*10;

            }

        }
    }

    //________________________________________________________________
    void Decoration::reconfigure()
    {
        
        m_internalSettings = SettingsProvider::self()->internalSettings( this );
        
        KSharedConfig::Ptr config = KSharedConfig::openConfig();
        
        // loads system ScaleFactor from ~/.config/kdeglobals
        const KConfigGroup cgKScreen(config, QStringLiteral("KScreen"));
        m_systemScaleFactor = cgKScreen.readEntry("ScaleFactor", 1.0f);
        
        setScaledCornerRadius();
        setScaledTitleBarTopBottomMargins();
        setScaledTitleBarSideMargins();
        setSystemAccentColors();
        
        if( m_internalSettings->buttonShape() == InternalSettings::EnumButtonShape::ShapeFullHeightRectangle
            || m_internalSettings->buttonShape() == InternalSettings::EnumButtonShape::ShapeFullHeightRoundedRectangle
        ) m_buttonSize = ButtonSize::FullHeight;
        else if( m_internalSettings->buttonShape() == InternalSettings::EnumButtonShape::ShapeLargeCircle)
            m_buttonSize = ButtonSize::Large;
        else m_buttonSize = ButtonSize::Small;
        
        const KConfigGroup cg(config, QStringLiteral("KDE"));
        
        m_colorSchemeHasHeaderColor =  KColorScheme::isColorSetSupported(config, KColorScheme::Header);
        //m_toolsAreaWillBeDrawn = ( m_colorSchemeHasHeaderColor && ( settings()->borderSize() == KDecoration2::BorderSize::None || settings()->borderSize() == KDecoration2::BorderSize::NoSides ) ); 
        m_toolsAreaWillBeDrawn = ( m_colorSchemeHasHeaderColor ); 
        
        // animation
        if( m_internalSettings->animationsEnabled() ) {
            qreal animationsDurationFactorRelativeSystem = 1;
            if ( m_internalSettings->animationsSpeedRelativeSystem() < 0 ) animationsDurationFactorRelativeSystem = ( -m_internalSettings->animationsSpeedRelativeSystem() + 2 ) / 2.0f;
            else if ( m_internalSettings->animationsSpeedRelativeSystem() > 0 ) animationsDurationFactorRelativeSystem = 1 / ((m_internalSettings->animationsSpeedRelativeSystem() + 2) / 2.0f); 
            m_animation->setDuration( cg.readEntry("AnimationDurationFactor", 1.0f) * 150.0f * animationsDurationFactorRelativeSystem);
            m_shadowAnimation->setDuration( cg.readEntry("AnimationDurationFactor", 1.0f) * 150.0f * animationsDurationFactorRelativeSystem );
        } else {
            m_animation->setDuration(0);
            m_shadowAnimation->setDuration(0);
        }

        setAddedTitleBarOpacity();

        // borders
        recalculateBorders();

        updateOpaque();
        updateBlur();

        // shadow
        updateShadow();

        // size grip
        if( hasNoBorders() && m_internalSettings->drawSizeGrip() ) createSizeGrip();
        else deleteSizeGrip();;
    }

    //________________________________________________________________
    void Decoration::recalculateBorders()
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        auto s = settings();
        
        //setScaledTitleBarTopBottomMargins();
        
        // left, right and bottom borders
        const int left   = isLeftEdge() ? 0 : borderSize();
        const int right  = isRightEdge() ? 0 : borderSize();
        const int bottom = (c->isShaded() || isBottomEdge()) ? 0 : borderSize(true);                

        int top = 0;
        if( hideTitleBar() ) top = bottom;
        else {

            QFontMetrics fm(s->font());
            top += qMax(fm.height(), iconHeight() );

            // padding below
            top += titleBarSeparatorHeight();
            top += (m_scaledTitleBarTopMargin + m_scaledTitleBarBottomMargin);

        }

        setBorders(QMargins(left, top, right, bottom));

        // extended sizes
        const int extSize = s->largeSpacing();
        int extSides = 0;
        int extBottom = 0;
        int extTop = 0;
        
        
        //Add extended resize handles for Full-sized Rectangle highlight as they cannot overlap with larger full-sized buttons
        if( m_buttonSize == ButtonSize::FullHeight || m_buttonSize == ButtonSize::Large){
            if( !isMaximizedVertically() ) extTop = extSize; 
        }
        
        if( hasNoBorders() )
        {
            if( !isMaximizedHorizontally() ) extSides = extSize;
            if( !isMaximizedVertically() ) { 
                extBottom = extSize;
            }

        } else if( ( hasNoSideBorders() || (m_internalSettings->titlebarSideMargins() == 0) ) && !isMaximizedHorizontally() ) {

            extSides = extSize;
        } 

        setResizeOnlyBorders(QMargins(extSides, extTop, extSides, extBottom));
    }

    //________________________________________________________________
    void Decoration::createButtons()
    {
        m_leftButtons = new KDecoration2::DecorationButtonGroup(KDecoration2::DecorationButtonGroup::Position::Left, this, &Button::create);
        m_rightButtons = new KDecoration2::DecorationButtonGroup(KDecoration2::DecorationButtonGroup::Position::Right, this, &Button::create);
        updateButtonsGeometry();
    }

    //________________________________________________________________
    void Decoration::updateButtonsGeometryDelayed()
    { QTimer::singleShot( 0, this, &Decoration::updateButtonsGeometry ); }

    //________________________________________________________________
    void Decoration::updateButtonsGeometry()
    {
        const auto s = settings();
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        setScaledTitleBarSideMargins();
        
        // adjust button position
        int bHeight;
        int bWidth=0;
        int iconHeight = this->iconHeight();
        int iconWidth = iconHeight;
        int verticalIconOffset=0;
        int horizontalIconOffsetLeftButtons=0;
        int horizontalIconOffsetRightButtons=0;
        
        if( m_buttonSize == ButtonSize::FullHeight ) {
            bHeight = borderTop();
            bHeight -= titleBarSeparatorHeight();
            
            verticalIconOffset = m_scaledTitleBarTopMargin + (captionHeight()-iconHeight)/2;
        } else if ( m_buttonSize == ButtonSize::Large ) {
            bHeight = borderTop();
            bHeight -= titleBarSeparatorHeight();
            verticalIconOffset = m_scaledTitleBarTopMargin + (captionHeight()-iconHeight)/2;
        } else {   
            bHeight = captionHeight() + (isTopEdge() ? m_scaledTitleBarTopMargin:0);
            verticalIconOffset = (isTopEdge() ? m_scaledTitleBarTopMargin:0) + (captionHeight()-iconHeight)/2;
        }
        
        foreach( const QPointer<KDecoration2::DecorationButton>& button, m_leftButtons->buttons() )
        {
            
            if ( m_buttonSize == ButtonSize::FullHeight ) {
                bWidth = iconHeight + s->smallSpacing()*m_internalSettings->fullHeightButtonWidthMarginLeft();
                horizontalIconOffsetLeftButtons = s->smallSpacing()*m_internalSettings->fullHeightButtonWidthMarginLeft() / 2;
                static_cast<Button*>( button.data() )->setFullHeightBackgroundVisibleSize( QSizeF( bWidth, bHeight ) );
            } else if ( m_buttonSize == ButtonSize::Large ) {
                bWidth = bHeight;
                horizontalIconOffsetLeftButtons = (bWidth - iconHeight)/2;
            } else {
                bWidth = iconHeight;
                horizontalIconOffsetLeftButtons = 0;
            }
            button.data()->setGeometry( QRectF( QPoint( 0, 0 ), QSizeF( bWidth, bHeight ) ) );
            static_cast<Button*>( button.data() )->setIconOffset( QPointF( horizontalIconOffsetLeftButtons, verticalIconOffset ) );
            static_cast<Button*>( button.data() )->setIconSize( QSize( iconWidth, iconWidth ) );
        }
        
        foreach( const QPointer<KDecoration2::DecorationButton>& button, m_rightButtons->buttons() )
        {
            
            if ( m_buttonSize == ButtonSize::FullHeight ) {
                bWidth = iconHeight + s->smallSpacing()*m_internalSettings->fullHeightButtonWidthMarginRight();
                horizontalIconOffsetRightButtons = s->smallSpacing()*m_internalSettings->fullHeightButtonWidthMarginRight() / 2;
                static_cast<Button*>( button.data() )->setFullHeightBackgroundVisibleSize( QSizeF( bWidth, bHeight ) );
            } else if ( m_buttonSize == ButtonSize::Large ) {
                bWidth = bHeight;
                horizontalIconOffsetRightButtons = (bWidth - iconHeight)/2;
            } else {
                bWidth = iconHeight;
                horizontalIconOffsetRightButtons = 0;
            }
            button.data()->setGeometry( QRectF( QPoint( 0, 0 ), QSizeF( bWidth, bHeight ) ) );
            static_cast<Button*>( button.data() )->setIconOffset( QPointF( horizontalIconOffsetRightButtons, verticalIconOffset ) );
            static_cast<Button*>( button.data() )->setIconSize( QSize( iconWidth, iconWidth ) );
        }

        // left buttons
        if( !m_leftButtons->buttons().isEmpty() )
        {

            // spacing
            m_leftButtons->setSpacing(s->smallSpacing()*m_internalSettings->buttonSpacingLeft());

            // padding
            int vPadding;
            if( m_buttonSize == ButtonSize::FullHeight ) vPadding = 0;
            else if(m_buttonSize == ButtonSize::Large ) vPadding = 0;
            else vPadding = isTopEdge() ? 0 : m_scaledTitleBarTopMargin;
            const int hPadding = m_scaledTitleBarSideMargins;
            
            auto firstButton = static_cast<Button*>( m_leftButtons->buttons().front().data() );
            if( isLeftEdge() )
            {
                // add offsets on the side buttons, to preserve padding, but satisfy Fitts law
                firstButton->setGeometry( QRectF( QPoint( 0, 0 ), QSizeF( firstButton->geometry().width() + hPadding, firstButton->geometry().height() ) ) );
                firstButton->setHorizontalIconOffset( horizontalIconOffsetLeftButtons + hPadding );
                firstButton->setLargeOrFullHeightVisibleBackgroundOffset( QPointF( hPadding, 0) );
                firstButton->setFlag( Button::FlagFirstInList );
                
                m_leftButtons->setPos(QPointF(0, vPadding));

            } else {
                m_leftButtons->setPos(QPointF(hPadding + borderLeft(), vPadding));
                firstButton->setLargeOrFullHeightVisibleBackgroundOffset( QPointF( 0, 0) );
            }

        }

        // right buttons
        if( !m_rightButtons->buttons().isEmpty() )
        {
            // spacing
            m_rightButtons->setSpacing(s->smallSpacing()*m_internalSettings->buttonSpacingRight());

            // padding
            int vPadding;
            if( m_buttonSize == ButtonSize::FullHeight ) vPadding = 0;
            else if(m_buttonSize == ButtonSize::Large ) vPadding = 0;
            else vPadding = isTopEdge() ? 0 : m_scaledTitleBarTopMargin;
            const int hPadding = m_scaledTitleBarSideMargins;
            
            auto lastButton = static_cast<Button*>( m_rightButtons->buttons().back().data() );
            if( isRightEdge() )
            {
                lastButton->setGeometry( QRectF( QPoint( 0, 0 ), QSizeF( lastButton->geometry().width() + hPadding, lastButton->geometry().height() ) ) );
                lastButton->setFlag( Button::FlagLastInList );

                m_rightButtons->setPos(QPointF(size().width() - m_rightButtons->geometry().width(), vPadding));

            } else m_rightButtons->setPos(QPointF(size().width() - m_rightButtons->geometry().width() - hPadding - borderRight(), vPadding));

        }

        update();

    }

    //________________________________________________________________
    void Decoration::paint(QPainter *painter, const QRect &repaintRegion)
    {
        // TODO: optimize based on repaintRegion
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        auto s = settings();
        
        calculateWindowAndTitleBarShapes();
        
        // paint background
        if( !c->isShaded() )
        {
            painter->fillRect(rect(), Qt::transparent);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            
            QColor windowBorderColor;
            if( m_internalSettings->useTitlebarColorForAllBorders() ) {
                windowBorderColor = titleBarColorWithAddedTransparency();
            } else windowBorderColor = c->color( c->isActive() ? ColorGroup::Active : ColorGroup::Inactive, ColorRole::Frame );
            
            painter->setBrush( windowBorderColor );
            
            QPainterPath clipRect;
            // use clipRect for clipping away the top part
            if( !hideTitleBar() ) 
            {
                clipRect.addRect(0, borderTop(), size().width(), size().height() - borderTop());
                //clip off the titlebar and draw bottom part
                QPainterPath windowPathMinusTitleBar = m_windowPath->intersected(clipRect);
                painter->drawPath(windowPathMinusTitleBar);
            } else painter->drawPath(*m_windowPath);
            
            painter->restore();
        }
        
        
        if( !hideTitleBar() ){
            paintTitleBar(painter, repaintRegion);
        }

        if( hasBorders() && !s->isAlphaChannelSupported() )
        {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setBrush( Qt::NoBrush );
            painter->setPen( c->isActive() ?
                c->color( ColorGroup::Active, ColorRole::TitleBar ):
                c->color( ColorGroup::Inactive, ColorRole::Foreground ) );

            painter->drawRect( rect().adjusted( 0, 0, -1, -1 ) );
            painter->restore();
        }
        
    }
    
    void Decoration::calculateWindowAndTitleBarShapes(const bool windowShapeOnly)
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        auto s = settings();
        
        if( !windowShapeOnly || c->isShaded() )
        {
            //set titleBar geometry and path
            m_titleRect = QRect(QPoint(0, 0), QSize(size().width(), borderTop()));
            m_titleBarPath->clear(); //clear the path for subsequent calls to this function
            if( isMaximized() || !s->isAlphaChannelSupported() )
            {
                m_titleBarPath->addRect(m_titleRect);

            } else if( c->isShaded() ) {
                m_titleBarPath->addRoundedRect(m_titleRect, m_scaledCornerRadius, m_scaledCornerRadius);

            } else {
                QPainterPath clipRect;
                clipRect.addRect(m_titleRect);

                // the rect is made a little bit larger to be able to clip away the rounded corners at the bottom and sides
                m_titleBarPath->addRoundedRect(m_titleRect.adjusted(
                    isLeftEdge() ? -m_scaledCornerRadius:0,
                    isTopEdge() ? -m_scaledCornerRadius:0,
                    isRightEdge() ? m_scaledCornerRadius:0,
                    m_scaledCornerRadius),
                    m_scaledCornerRadius, m_scaledCornerRadius);

                *m_titleBarPath = m_titleBarPath->intersected(clipRect);
            }
        }
        
        //set windowPath
        m_windowPath->clear(); //clear the path for subsequent calls to this function
        if( !c->isShaded() )
        {
            if( s->isAlphaChannelSupported() && !isMaximized() ) m_windowPath->addRoundedRect(rect(), m_scaledCornerRadius, m_scaledCornerRadius);
            else m_windowPath->addRect( rect() );
            
        } else {
            *m_windowPath = *m_titleBarPath;
        }
        
    }

    //________________________________________________________________
    void Decoration::paintTitleBar(QPainter *painter, const QRect &repaintRegion)
    {
        const auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        if ( !m_titleRect.intersects(repaintRegion) ) return;

        painter->save();
        painter->setPen(Qt::NoPen);

        QColor titleBarColor( titleBarColorWithAddedTransparency() );
        
        // render a linear gradient on title area
        if( c->isActive() && m_internalSettings->drawBackgroundGradient() )
        {            
            QLinearGradient gradient( 0, 0, 0, m_titleRect.height() );
            gradient.setColorAt(0.0, titleBarColor.lighter( 120 ) );
            gradient.setColorAt(0.8, titleBarColor);
            painter->setBrush(gradient);

        } else {
            painter->setBrush( titleBarColor );
        }
        

        auto s = settings();
        
        painter->drawPath(*m_titleBarPath);

        const QColor titleBarSeparatorColor( this->titleBarSeparatorColor() );
        if( titleBarSeparatorHeight() && titleBarSeparatorColor.isValid() )
        {
            // outline
            painter->setRenderHint( QPainter::Antialiasing, false );
            painter->setBrush( Qt::NoBrush );
            QPen p(titleBarSeparatorColor);
            p.setWidthF(titleBarSeparatorHeight());
            p.setCosmetic(true);
            painter->setPen( p );
            
            qreal separatorYOffset = 0.5;
            if( KWindowSystem::isPlatformWayland() ) separatorYOffset *= painter->device()->devicePixelRatioF();
            qreal separatorYCoOrd = qreal(m_titleRect.bottom()) - titleBarSeparatorHeight()/2 + separatorYOffset;
            if ( m_internalSettings->useTitlebarColorForAllBorders() ){
                painter->drawLine( 
                    QPointF( m_titleRect.bottomLeft().x() + borderLeft() + titleBarSeparatorHeight()/2, separatorYCoOrd ),
                    QPointF( m_titleRect.bottomRight().x() - borderRight() - titleBarSeparatorHeight()/2, separatorYCoOrd )
                );
            } else{
                painter->drawLine( 
                    QPointF( m_titleRect.bottomLeft().x(), separatorYCoOrd ),
                    QPointF( m_titleRect.bottomRight().x(), separatorYCoOrd )
                );
            } 
        }

        painter->restore();

        // draw caption
        painter->setFont(s->font());
        painter->setPen( fontColor() );
        const auto cR = captionRect();
        const QString caption = painter->fontMetrics().elidedText(c->caption(), Qt::ElideMiddle, cR.first.width());
        painter->drawText(cR.first, cR.second | Qt::TextSingleLine, caption);

        // draw all buttons
        m_leftButtons->paint(painter, repaintRegion);
        m_rightButtons->paint(painter, repaintRegion);
    }

    //________________________________________________________________
    int Decoration::iconHeight() const
    {
        const int baseSize = settings()->gridUnit();
        switch( m_internalSettings->iconSize() )
        {
            case InternalSettings::IconTiny: return baseSize;
            case InternalSettings::IconSmall: return baseSize*1.5;
            default:
            case InternalSettings::IconDefault: return baseSize*2;
            case InternalSettings::IconLarge: return baseSize*2.5;
            case InternalSettings::IconVeryLarge: return baseSize*3.5;
        }

    }
    
    
    //________________________________________________________________
    int Decoration::captionHeight() const
    { 
 
        return hideTitleBar() ? borderTop() : borderTop() - m_scaledTitleBarTopMargin - m_scaledTitleBarBottomMargin - titleBarSeparatorHeight();
    }

    //________________________________________________________________
    QPair<QRect,Qt::Alignment> Decoration::captionRect() const
    {
        if( hideTitleBar() ) return qMakePair( QRect(), Qt::AlignCenter );
        else {

           auto c = client().toStrongRef();
           Q_ASSERT(c);
           
            //determine padding of title caption using other padding values
            int leftButtonPadding = settings()->smallSpacing()*m_internalSettings->buttonSpacingLeft();
            int rightButtonPadding = settings()->smallSpacing()*m_internalSettings->buttonSpacingRight();
            if( m_buttonSize == ButtonSize::FullHeight ){
                leftButtonPadding += settings()->smallSpacing()*m_internalSettings->fullHeightButtonWidthMarginLeft();
                rightButtonPadding += settings()->smallSpacing()*m_internalSettings->fullHeightButtonWidthMarginRight();
            }

            int padding = 0;
            if(leftButtonPadding == 0 || rightButtonPadding == 0) padding = qMax(leftButtonPadding,rightButtonPadding);
            else padding = qMin(leftButtonPadding,rightButtonPadding);

            const int leftOffset = m_leftButtons->buttons().isEmpty() ?
                padding:
                m_leftButtons->geometry().x() + m_leftButtons->geometry().width() + padding;

            const int rightOffset = m_rightButtons->buttons().isEmpty() ?
                padding :
                size().width() - m_rightButtons->geometry().x() + padding;

            const int yOffset = m_scaledTitleBarTopMargin;
            const QRect maxRect( leftOffset, yOffset, size().width() - leftOffset - rightOffset, captionHeight() );

            switch( m_internalSettings->titleAlignment() )
            {
                case InternalSettings::AlignLeft:
                return qMakePair( maxRect, Qt::AlignVCenter|Qt::AlignLeft );

                case InternalSettings::AlignRight:
                return qMakePair( maxRect, Qt::AlignVCenter|Qt::AlignRight );

                case InternalSettings::AlignCenter:
                return qMakePair( maxRect, Qt::AlignCenter );

                default:
                case InternalSettings::AlignCenterFullWidth:
                {

                    // full caption rect
                    const QRect fullRect = QRect( 0, yOffset, size().width(), captionHeight() );
                    QRect boundingRect( settings()->fontMetrics().boundingRect( c->caption()).toRect() );

                    // text bounding rect
                    boundingRect.setTop( yOffset );
                    boundingRect.setHeight( captionHeight() );
                    boundingRect.moveLeft( ( size().width() - boundingRect.width() )/2 );

                    if( boundingRect.left() < leftOffset ) return qMakePair( maxRect, Qt::AlignVCenter|Qt::AlignLeft );
                    else if( boundingRect.right() > size().width() - rightOffset ) return qMakePair( maxRect, Qt::AlignVCenter|Qt::AlignRight );
                    else return qMakePair(fullRect, Qt::AlignCenter);

                }

            }

        }

    }

    //________________________________________________________________
    void Decoration::updateShadow( const bool force )
    {
        auto s = settings();
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        // Animated case, no cached shadow object
        if ( (m_shadowAnimation->state() == QAbstractAnimation::Running) && (m_shadowOpacity != 0.0) && (m_shadowOpacity != 1.0) )
        {
            setShadow(createShadowObject(0.5 + m_shadowOpacity * 0.5));
            return;
        }
                
        if ( force
                || g_shadowSizeEnum != m_internalSettings->shadowSize()
                || g_shadowStrength != m_internalSettings->shadowStrength()
                || g_shadowColor != m_internalSettings->shadowColor()
                || !(qAbs(g_cornerRadius - m_scaledCornerRadius) < 0.001)
                || !(qAbs(g_systemScaleFactor - m_systemScaleFactor) < 0.001)
                || g_hasNoBorders != hasNoBorders() 
                || g_thinWindowOutlineStyle != m_internalSettings->thinWindowOutlineStyle()
                || g_thinWindowOutlineCustomColor != m_internalSettings->thinWindowOutlineCustomColor()
                || g_thinWindowOutlineThickness != m_internalSettings->thinWindowOutlineThickness()
        ){
            g_sShadow.clear();
            g_sShadowInactive.clear();
            g_shadowSizeEnum = m_internalSettings->shadowSize();
            g_shadowStrength = m_internalSettings->shadowStrength();
            g_shadowColor = m_internalSettings->shadowColor();
            g_cornerRadius = m_scaledCornerRadius;
            g_systemScaleFactor = m_systemScaleFactor;
            g_hasNoBorders = hasNoBorders();
            g_thinWindowOutlineStyle = m_internalSettings->thinWindowOutlineStyle();
            g_thinWindowOutlineCustomColor = m_internalSettings->thinWindowOutlineCustomColor();
            g_thinWindowOutlineThickness = m_internalSettings->thinWindowOutlineThickness();
        }

        auto& shadow = (c->isActive()) ? g_sShadow : g_sShadowInactive;
        if ( !shadow )
        {
            shadow = createShadowObject( c->isActive() ? 1.0 : 0.5 );
        }
        setShadow(shadow);
    }

    //________________________________________________________________
    QSharedPointer<KDecoration2::DecorationShadow> Decoration::createShadowObject( const float strengthScale )
    {
        const CompositeShadowParams params = lookupShadowParams(m_internalSettings->shadowSize());
          if ( m_internalSettings->shadowSize() == InternalSettings::EnumShadowSize::ShadowNone 
              && m_internalSettings->thinWindowOutlineStyle() == InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineNone )
          {
              return nullptr;
          }

          auto withOpacity = [](const QColor& color, qreal opacity) -> QColor {
              QColor c(color);
              c.setAlphaF(opacity * c.alphaF());
              return c;
          };


          const QSize boxSize = BoxShadowRenderer::calculateMinimumBoxSize(params.shadow1.radius)
              .expandedTo(BoxShadowRenderer::calculateMinimumBoxSize(params.shadow2.radius));
              
          auto c = client().toStrongRef();
          Q_ASSERT(c);
          
          qreal outlinePenWidth = m_internalSettings->thinWindowOutlineThickness();
          
          //the overlap between the thin window outline and behind the window in unscaled pixels.
          //This is necessary for the thin window outline to sit flush with the window on Wayland, 
          //and also makes sure that the anti-aliasing blends properly between the window and thin window outline
          qreal outlineOverlap = 0.5; 
          
          //scale outline
          //We can't get the DPR for Wayland from KDecoration/KWin but can work around this as Wayland will auto-scale if you don't use a cosmetic pen. On X11 this does not happen but we can use the system-set scaling value directly.
          if( KWindowSystem::isPlatformX11() ){
              outlinePenWidth *= m_systemScaleFactor;
              outlineOverlap *= m_systemScaleFactor;
          }
          
          BoxShadowRenderer shadowRenderer;
          
          shadowRenderer.setBorderRadius(m_scaledCornerRadius + 0.5);
          shadowRenderer.setBoxSize(boxSize);
          const qreal strength = m_internalSettings->shadowStrength() / 255.0 * strengthScale;
          shadowRenderer.addShadow(params.shadow1.offset, params.shadow1.radius,
              withOpacity(m_internalSettings->shadowColor(), params.shadow1.opacity * strength));
          shadowRenderer.addShadow(params.shadow2.offset, params.shadow2.radius,
              withOpacity(m_internalSettings->shadowColor(), params.shadow2.opacity * strength));

          QImage shadowTexture = shadowRenderer.render();

          QPainter painter(&shadowTexture);
          painter.setRenderHint(QPainter::Antialiasing);

          const QRect outerRect = shadowTexture.rect();

          QRect boxRect(QPoint(0, 0), boxSize);
          boxRect.moveCenter(outerRect.center());

          // Mask out inner rect.
          const QMargins padding = QMargins(
              boxRect.left() - outerRect.left() - Metrics::Shadow_Overlap - params.offset.x(),
              boxRect.top() - outerRect.top() - Metrics::Shadow_Overlap - params.offset.y(),
              outerRect.right() - boxRect.right() - Metrics::Shadow_Overlap + params.offset.x(),
              outerRect.bottom() - boxRect.bottom() - Metrics::Shadow_Overlap + params.offset.y());
          const QRectF innerRect = outerRect - padding;

          painter.setPen(Qt::NoPen);
          painter.setBrush(Qt::black);
          painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
          
          
          QRectF innerRectPotentiallyTaller = innerRect;
          
          QPainterPath innerRectPath;
          innerRectPath.addRect(innerRect);
          
          // if we have no borders we don't have rounded bottom corners, so make a taller rounded rectangle and clip off its bottom
          if ( hasNoBorders() && !c->isShaded() ) innerRectPotentiallyTaller.adjust(0,0,0,m_scaledCornerRadius); 
          
          QPainterPath roundedRectMask;
          roundedRectMask.addRoundedRect(
            innerRectPotentiallyTaller,
            m_scaledCornerRadius + 0.5,
            m_scaledCornerRadius + 0.5);
          
          if ( hasNoBorders() && !c->isShaded() ) roundedRectMask = roundedRectMask.intersected(innerRectPath);


          painter.drawPath(roundedRectMask);

        
          if( m_internalSettings->thinWindowOutlineStyle() != InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineNone) 
          {
            qreal outlineAdjustment = outlinePenWidth/2 - outlineOverlap;
            QRectF outlineRect;
            outlineRect = innerRect.adjusted(-outlineAdjustment, -outlineAdjustment, outlineAdjustment, outlineAdjustment); //make thin window outline rect larger so most is outside the window, except for a 0.5px scaled overlap
            QPainterPath outlineRectPath;
            outlineRectPath.addRect(outlineRect);

            QRectF outlineRectPotentiallyTaller = outlineRect;

            // if we have no borders we don't have rounded bottom corners, so make a taller rounded rectangle and clip off its bottom
            if ( hasNoBorders() && !c->isShaded() ) outlineRectPotentiallyTaller = outlineRect.adjusted(0,0,0,m_scaledCornerRadius);

            // Draw Thin window outline
            QPen p;
            if ( m_internalSettings->thinWindowOutlineStyle() == InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineContrastTitleBarText )
                p.setColor(withOpacity(fontColor(), 0.25));
            else if ( m_internalSettings->thinWindowOutlineStyle() == InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineAccentColor )
                p.setColor( accentedWindowOutlineColor() );
            else if( m_internalSettings->thinWindowOutlineStyle() == InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineAccentWithContrast)
                p.setColor( fontMixedAccentWindowOutlineColor() );
            else if ( m_internalSettings->thinWindowOutlineStyle() == InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineCustomColor )
                p.setColor( accentedWindowOutlineColor( m_internalSettings->thinWindowOutlineCustomColor() ) );
            else if( m_internalSettings->thinWindowOutlineStyle() == InternalSettings::EnumThinWindowOutlineStyle::WindowOutlineCustomWithContrast)
                p.setColor( fontMixedAccentWindowOutlineColor( m_internalSettings->thinWindowOutlineCustomColor() ) );
            else p.setColor(withOpacity(m_internalSettings->shadowColor(), 0.2 * strength)); //blend to shadow
            
            p.setWidthF(outlinePenWidth);
            painter.setPen(p);
            painter.setBrush(Qt::NoBrush);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            
            QPainterPath roundedRectOutline;
            qreal cornerRadius;
                
            if( m_scaledCornerRadius < 0.05 ) cornerRadius = 0; //give a square corner for when corner radius is 0
            else cornerRadius = m_scaledCornerRadius + outlineAdjustment; //else round corner slightly more to account for pen width
            
            roundedRectOutline.addRoundedRect(
                outlineRectPotentiallyTaller,
                cornerRadius,
                cornerRadius);
            
            if ( hasNoBorders() && !c->isShaded() ) roundedRectOutline = roundedRectOutline.intersected(outlineRectPath);
            
            painter.drawPath(roundedRectOutline);
          }
          painter.end();

          auto ret = QSharedPointer<KDecoration2::DecorationShadow>::create();
          ret->setPadding(padding);
          ret->setInnerShadowRect(QRect(outerRect.center(), QSize(1, 1)));
          ret->setShadow(shadowTexture);
          return ret;
    }

    //_________________________________________________________________
    void Decoration::createSizeGrip()
    {

        // do nothing if size grip already exist
        if( m_sizeGrip ) return;

        #if BREEZE_HAVE_X11
        if( !QX11Info::isPlatformX11() ) return;

        // access client
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        if( !c ) return;

        if( c->windowId() != 0 )
        {
            m_sizeGrip = new SizeGrip( this );
            connect( c.data(), &KDecoration2::DecoratedClient::maximizedChanged, this, &Decoration::updateSizeGripVisibility );
            connect( c.data(), &KDecoration2::DecoratedClient::shadedChanged, this, &Decoration::updateSizeGripVisibility );
            connect( c.data(), &KDecoration2::DecoratedClient::resizeableChanged, this, &Decoration::updateSizeGripVisibility );
        }
        #endif
        
    }

    //_________________________________________________________________
    void Decoration::deleteSizeGrip()
    {
        if( m_sizeGrip )
        {
            m_sizeGrip->deleteLater();
            m_sizeGrip = nullptr;
        }
    }
    
    void Decoration::setScaledTitleBarTopBottomMargins()
    {
        // access client
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        qreal topBottomMargins;
        if( c->isMaximized() ){
            topBottomMargins = m_internalSettings->titlebarTopBottomMargins() * m_internalSettings->percentMaximizedTopBottomMargins()/100;
        } else {
            topBottomMargins = m_internalSettings->titlebarTopBottomMargins();
        }
        
        m_scaledTitleBarTopMargin = int( settings()->smallSpacing() * topBottomMargins );
        m_scaledTitleBarBottomMargin = m_scaledTitleBarTopMargin;
    }
    
    void Decoration::setScaledTitleBarSideMargins()
    {       
        m_scaledTitleBarSideMargins = int( qreal(m_internalSettings->titlebarSideMargins()) * qreal(settings()->smallSpacing() ) );
        
        //subtract any added margins from the side margin so the user doesn't need to adjust the side margins when changing border size
        //this makes the side margin relative to the border edge rather than the titlebar edge
        if( !isMaximizedHorizontally() ) m_scaledTitleBarSideMargins -= this->borderSize(false);

    }
    
    void Decoration::setAddedTitleBarOpacity()
    {
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        if ( c->isMaximized() && m_internalSettings->opaqueMaximizedTitlebars() ) {
            m_addedTitleBarOpacityActive = 1;
            m_addedTitleBarOpacityInactive = 1;
        } else {
            m_addedTitleBarOpacityActive = qreal( m_internalSettings->activeTitlebarOpacity() ) / 100;
            m_addedTitleBarOpacityInactive = qreal( m_internalSettings->inactiveTitlebarOpacity() ) / 100;
        }
        
    }
    
    void Decoration::setScaledCornerRadius()
    {
        m_scaledCornerRadius = m_internalSettings->cornerRadius() * settings()->smallSpacing();
    }
    
    void Decoration::updateOpaque()
    {
        // access client
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        int titleBarOpacityToAdd = c->isActive() ? m_internalSettings->activeTitlebarOpacity() : m_internalSettings->inactiveTitlebarOpacity();

        if( (m_internalSettings->opaqueMaximizedTitlebars() && c->isMaximized() )
            || ( titleBarOpacityToAdd == 100 && titleBarColor(true).alpha() == 255 )
        ){ //opaque titlebar colours
            if( c->isMaximized() ) setOpaque(true);
            else setOpaque(false);
        }
        else { //transparent titlebar colours
            setOpaque(false);
        }

    }

    void Decoration::updateBlur()
    {
        // access client
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        int titleBarOpacityToAdd = c->isActive() ? m_internalSettings->activeTitlebarOpacity() : m_internalSettings->inactiveTitlebarOpacity();

        //disable blur if the titlebar is opaque
        if( (m_internalSettings->opaqueMaximizedTitlebars() && c->isMaximized() )
            || ( titleBarOpacityToAdd == 100 && titleBarColor(true).alpha() == 255 )
        ){ //opaque titlebar colours
            setBlurRegion( QRegion() );
        }
        else { //transparent titlebar colours
            if( m_internalSettings->blurTransparentTitlebars() ){// enable blur
                calculateWindowAndTitleBarShapes(true); //refreshes m_windowPath
                setBlurRegion( QRegion( m_windowPath->toFillPolygon().toPolygon()) ) ;
            } else setBlurRegion( QRegion() );

        }
    }
    
    void Decoration::setSystemAccentColors()
    {       
        // access client
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        m_systemAccentColors = ColorTools::getSystemButtonColors( c->palette() );
    }
    
    qreal Decoration::titleBarSeparatorHeight() const
    {
        // access client
        auto c = client().toStrongRef();
        Q_ASSERT(c);
        
        if( m_internalSettings->drawTitleBarSeparator() && !c->isMaximized() && !c->isShaded() ) {
            return 1;
        } else return 0;
    }

} // namespace


#include "breezedecoration.moc"
