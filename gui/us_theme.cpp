//! \file us_theme.cpp
#include "us_theme.h"
#include "us_gui_settings.h"
#include "us_defines.h"

#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
#include <QStyleHints>
#endif

namespace
{
   // Remembers the configuration that produced the currently applied look, so
   // that the many US_Widgets constructors do not rebuild it over and over.
   QString  applied_signature;
   bool     watcher_installed = false;

   // The color scheme the desktop asked for the first time we looked.  It is
   // sampled before UltraScan installs its own palette, because on Qt versions
   // without QStyleHints::colorScheme() the system palette is the only hint we
   // have and our own palette would falsify it.
   int      sampled_system_scheme = -1;
}

// The two token tables.  These are *the* place to edit UltraScan's colors.
US_ThemeTokens US_Theme::tokens( Scheme s )
{
   US_ThemeTokens t;

   if ( s == Dark )
   {
      t.window        = QColor( 0x21, 0x26, 0x2d );
      t.windowAlt     = QColor( 0x2a, 0x30, 0x38 );
      t.windowText    = QColor( 0xe4, 0xe9, 0xee );
      t.mutedText     = QColor( 0x9b, 0xa6, 0xb2 );
      t.disabledText  = QColor( 0x6b, 0x76, 0x81 );

      t.base          = QColor( 0x17, 0x1b, 0x21 );
      t.baseAlt       = QColor( 0x1d, 0x22, 0x2a );
      t.baseReadOnly  = QColor( 0x26, 0x2c, 0x34 );
      t.text          = QColor( 0xe4, 0xe9, 0xee );

      t.button        = QColor( 0x2d, 0x33, 0x3b );
      t.buttonHover   = QColor( 0x39, 0x41, 0x4b );
      t.buttonPressed = QColor( 0x47, 0x50, 0x5b );
      t.buttonText    = QColor( 0xe4, 0xe9, 0xee );

      t.border        = QColor( 0x3d, 0x44, 0x4d );
      t.borderStrong  = QColor( 0x52, 0x5b, 0x66 );
      t.shadow        = QColor( 0x0d, 0x10, 0x14 );

      t.accent        = QColor( 0x1f, 0x6e, 0x88 );
      t.accentText    = QColor( 0xff, 0xff, 0xff );
      t.accentBright  = QColor( 0x4e, 0xb8, 0xd6 );
      t.attention     = QColor( 0xff, 0x8a, 0x7a );

      t.bannerBg      = QColor( 0x1f, 0x6e, 0x88 );
      t.bannerText    = QColor( 0xf2, 0xfa, 0xfd );

      t.tipBg         = QColor( 0x2a, 0x30, 0x38 );
      t.tipText       = QColor( 0xe4, 0xe9, 0xee );

      t.lcdBg         = QColor( 0x10, 0x16, 0x1d );
      t.lcdText       = QColor( 0x3d, 0xdc, 0x97 );
      t.lcdHi1        = QColor( 0x3d, 0xdc, 0x97 );
      t.lcdHi2        = QColor( 0x14, 0x58, 0x6b );
   }

   else
   {
      t.window        = QColor( 0xf2, 0xf4, 0xf7 );
      t.windowAlt     = QColor( 0xe8, 0xec, 0xf1 );
      t.windowText    = QColor( 0x1b, 0x27, 0x33 );
      t.mutedText     = QColor( 0x5a, 0x66, 0x72 );
      t.disabledText  = QColor( 0x98, 0xa2, 0xae );

      t.base          = QColor( 0xff, 0xff, 0xff );
      t.baseAlt       = QColor( 0xf7, 0xf9, 0xfb );
      t.baseReadOnly  = QColor( 0xe9, 0xed, 0xf1 );
      t.text          = QColor( 0x1b, 0x27, 0x33 );

      t.button        = QColor( 0xff, 0xff, 0xff );
      t.buttonHover   = QColor( 0xe4, 0xed, 0xf5 );
      t.buttonPressed = QColor( 0xd3, 0xdf, 0xea );
      t.buttonText    = QColor( 0x1b, 0x27, 0x33 );

      t.border        = QColor( 0xc9, 0xd2, 0xdb );
      t.borderStrong  = QColor( 0xa9, 0xb5, 0xc1 );
      t.shadow        = QColor( 0x8c, 0x99, 0xa6 );

      t.accent        = QColor( 0x0e, 0x6e, 0x8c );
      t.accentText    = QColor( 0xff, 0xff, 0xff );
      t.accentBright  = QColor( 0x0e, 0x6e, 0x8c );
      t.attention     = QColor( 0xc0, 0x39, 0x2b );

      t.bannerBg      = QColor( 0x0e, 0x6e, 0x8c );
      t.bannerText    = QColor( 0xff, 0xff, 0xff );

      t.tipBg         = QColor( 0x25, 0x31, 0x3d );
      t.tipText       = QColor( 0xf4, 0xf7, 0xfa );

      t.lcdBg         = QColor( 0x10, 0x16, 0x1d );
      t.lcdText       = QColor( 0x2f, 0xbf, 0x82 );
      t.lcdHi1        = QColor( 0x2f, 0xbf, 0x82 );
      t.lcdHi2        = QColor( 0x0e, 0x6e, 0x8c );
   }

   // The plot canvas stays dark in both schemes.  Many analysis programs set
   // a black canvas and bright curve colors explicitly, so a light canvas
   // would make UltraScan's plots inconsistent with each other.
   t.plotBg        = QColor( 0x10, 0x18, 0x22 );
   t.plotMajGrid   = QColor( 0x6a, 0x76, 0x83 );
   t.plotMinGrid   = QColor( 0x41, 0x4b, 0x57 );
   t.plotPicker    = QColor( 0xe8, 0xee, 0xf4 );
   t.plotCurve     = QColor( Qt::yellow );

   return t;
}

US_ThemeTokens US_Theme::tokens( void )
{
   return tokens( scheme() );
}

QString US_Theme::schemeSetting( void )
{
   QSettings settings( US3, "UltraScan" );
   return settings.value( "colorScheme", "auto" ).toString().toLower();
}

void US_Theme::set_schemeSetting( const QString& pref )
{
   QSettings settings( US3, "UltraScan" );

   if ( pref.compare( "auto", Qt::CaseInsensitive ) == 0 )
      settings.remove  ( "colorScheme" );
   else
      settings.setValue( "colorScheme", pref.toLower() );

   invalidate();
}

US_Theme::Scheme US_Theme::systemScheme( void )
{
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
   // Qt 6.5 and later know what the desktop asks for and keep the answer up
   // to date, so it can be queried every time.
   if ( QGuiApplication::styleHints() != nullptr )
   {
      const Qt::ColorScheme cs = QGuiApplication::styleHints()->colorScheme();

      if ( cs == Qt::ColorScheme::Dark )
         return Dark;

      if ( cs == Qt::ColorScheme::Light )
         return Light;
   }
#endif

   // Otherwise fall back to the lightness of the system window color.  It is
   // sampled only once, since after apply() the application palette is ours.
   if ( sampled_system_scheme < 0 )
   {
      if ( QGuiApplication::instance() == nullptr )
         return Light;

      const QColor win = QGuiApplication::palette().color( QPalette::Window );
      sampled_system_scheme = ( win.lightness() < 128 ) ? Dark : Light;
   }

   return ( sampled_system_scheme == Dark ) ? Dark : Light;
}

US_Theme::Scheme US_Theme::scheme( void )
{
   const QString pref = schemeSetting();

   if ( pref == "light" ) return Light;
   if ( pref == "dark"  ) return Dark;

   return systemScheme();
}

bool US_Theme::isDark( void )
{
   return ( scheme() == Dark );
}

QString US_Theme::defaultStyle( void )
{
   // Fusion is available on every platform Qt supports and is the only style
   // that renders identically on Linux, Windows and macOS.  It is also the
   // only built-in style that honors an application palette everywhere, which
   // is what makes the UltraScan color configuration work at all.
   return QString( "Fusion" );
}

QFont US_Theme::baseFont( void )
{
   return QFont( US_GuiSettings::fontFamily(), US_GuiSettings::fontSize() );
}

QPalette US_Theme::applicationPalette( void )
{
   const US_ThemeTokens t = tokens();

   // Start from the "Other Widgets" palette so that a user who redefines it
   // also restyles menus, dialogs and everything else that has no palette of
   // its own, then fill in the roles that palette does not cover.
   QPalette p = US_GuiSettings::normalColor();

   const QList< QPalette::ColorGroup > groups = QList< QPalette::ColorGroup >()
      << QPalette::Active << QPalette::Inactive << QPalette::Disabled;

   for ( int ii = 0; ii < groups.size(); ii++ )
   {
      const QPalette::ColorGroup g = groups[ ii ];

      p.setColor( g, QPalette::AlternateBase, t.baseAlt      );
      p.setColor( g, QPalette::ToolTipBase  , t.tipBg        );
      p.setColor( g, QPalette::ToolTipText  , t.tipText      );
      p.setColor( g, QPalette::Link         , t.accentBright );
      p.setColor( g, QPalette::LinkVisited  , t.accentBright );
   }

   p.setColor( QPalette::Active  , QPalette::PlaceholderText, t.mutedText    );
   p.setColor( QPalette::Inactive, QPalette::PlaceholderText, t.mutedText    );
   p.setColor( QPalette::Disabled, QPalette::PlaceholderText, t.disabledText );

   return p;
}

QString US_Theme::styleSheet( void )
{
   const US_ThemeTokens t = tokens();

   // What may and what may not appear in this style sheet
   // -----------------------------------------------------
   // A style sheet rule that touches a widget's box (border, background,
   // padding) takes the painting of that widget away from the style, and Qt
   // then ignores the palette that was set on the widget itself - inside a
   // rule, palette() always resolves against the *application* palette.
   //
   // Everything the user can recolor in the "Color Configuration" panel
   // (labels, banners, push buttons, edit fields, item views, LCDs, plots) is
   // therefore left entirely to its QPalette and is not mentioned here.  The
   // rules below only cover chrome that has no palette of its own, so that
   // shape and hover feedback can be modernized without taking the color
   // settings away from the user.
   QString qss;

   // ---- Tool tips -------------------------------------------------------
   qss += QString(
      "QToolTip {"
      " color: %1; background-color: %2; border: 1px solid %3;"
      " border-radius: 4px; padding: 4px 6px; }\n" )
      .arg( t.tipText.name(), t.tipBg.name(), t.border.name() );

   // ---- Check boxes and radio buttons -----------------------------------
   // The style outlines its indicators with a shade of the window color,
   // which all but disappears on a dark background.
   qss +=
      "QCheckBox::indicator, QRadioButton::indicator, QGroupBox::indicator {"
      " width: 14px; height: 14px;"
      " background-color: palette(base); border: 1px solid palette(mid); }\n"
      "QCheckBox::indicator, QGroupBox::indicator { border-radius: 3px; }\n"
      "QRadioButton::indicator { border-radius: 8px; }\n"
      "QCheckBox::indicator:hover, QRadioButton::indicator:hover,"
      " QGroupBox::indicator:hover { border-color: palette(highlight); }\n"
      "QCheckBox::indicator:checked, QGroupBox::indicator:checked {"
      " background-color: palette(highlight); border-color: palette(highlight);"
      " image: url(:/images/us_check.svg); }\n"
      "QCheckBox::indicator:indeterminate {"
      " background-color: palette(highlight); border-color: palette(highlight);"
      " image: url(:/images/us_check_dash.svg); }\n"
      "QRadioButton::indicator:checked {"
      " background-color: palette(highlight); border-color: palette(highlight);"
      " image: url(:/images/us_radio_dot.svg); }\n"
      "QCheckBox::indicator:disabled, QRadioButton::indicator:disabled,"
      " QGroupBox::indicator:disabled {"
      " background-color: palette(button); border-color: palette(mid); }\n"
      "QCheckBox::indicator:checked:disabled,"
      " QRadioButton::indicator:checked:disabled {"
      " background-color: palette(mid); border-color: palette(mid); }\n";

   // ---- Tabs ------------------------------------------------------------
   qss +=
      "QTabWidget::pane {"
      " border: 1px solid palette(mid); border-radius: 6px; top: -1px; }\n"
      "QTabBar::tab {"
      " background: transparent; color: palette(window-text);"
      " border: none; border-bottom: 2px solid transparent;"
      " padding: 6px 12px; margin-right: 2px; }\n"
      "QTabBar::tab:selected {"
      " color: palette(highlight); border-bottom: 2px solid palette(highlight); }\n"
      "QTabBar::tab:!selected:hover { color: palette(highlight); }\n";

   // ---- Group boxes -----------------------------------------------------
   qss +=
      "QGroupBox {"
      " border: 1px solid palette(mid); border-radius: 6px;"
      " margin-top: 9px; padding-top: 6px; }\n"
      "QGroupBox::title {"
      " subcontrol-origin: margin; subcontrol-position: top left;"
      " left: 9px; padding: 0px 4px; }\n";

   // ---- Progress bars ---------------------------------------------------
   qss +=
      "QProgressBar {"
      " background-color: palette(base); border: 1px solid palette(mid);"
      " border-radius: 6px; text-align: center; }\n"
      "QProgressBar::chunk {"
      " background-color: palette(highlight); border-radius: 5px;"
      " margin: 1px; }\n";

   // ---- Item view headers -----------------------------------------------
   qss +=
      "QHeaderView::section {"
      " background-color: palette(button); color: palette(button-text);"
      " border: none; border-right: 1px solid palette(mid);"
      " border-bottom: 1px solid palette(mid); padding: 4px 6px; }\n";

   // ---- Menus -----------------------------------------------------------
   qss +=
      "QMenuBar {"
      " background-color: palette(window); color: palette(window-text);"
      " border-bottom: 1px solid palette(mid); }\n"
      "QMenuBar::item {"
      " background: transparent; padding: 5px 10px; border-radius: 4px; }\n"
      "QMenuBar::item:selected {"
      " background-color: palette(highlight); color: palette(highlighted-text); }\n"
      "QMenu {"
      " background-color: palette(base); color: palette(text);"
      " border: 1px solid palette(mid); border-radius: 4px; padding: 4px; }\n"
      "QMenu::item { padding: 5px 22px; border-radius: 4px; }\n"
      "QMenu::item:selected {"
      " background-color: palette(highlight); color: palette(highlighted-text); }\n"
      "QMenu::separator {"
      " height: 1px; background: palette(mid); margin: 4px 8px; }\n";

   qss += QString(
      "QMenu::item:disabled, QMenuBar::item:disabled { color: %1; }\n" )
      .arg( t.disabledText.name() );

   // ---- Scroll bars -----------------------------------------------------
   qss +=
      "QScrollBar:vertical { background: transparent; width: 12px; margin: 0px; }\n"
      "QScrollBar:horizontal { background: transparent; height: 12px; margin: 0px; }\n"
      "QScrollBar::handle:vertical {"
      " background: palette(mid); border-radius: 4px; margin: 2px;"
      " min-height: 24px; }\n"
      "QScrollBar::handle:horizontal {"
      " background: palette(mid); border-radius: 4px; margin: 2px;"
      " min-width: 24px; }\n"
      "QScrollBar::handle:hover { background: palette(dark); }\n"
      "QScrollBar::add-line, QScrollBar::sub-line {"
      " width: 0px; height: 0px; border: none; background: none; }\n"
      "QScrollBar::add-page, QScrollBar::sub-page { background: none; }\n";

   // ---- Splitters -------------------------------------------------------
   qss +=
      "QSplitter::handle { background: palette(mid); }\n"
      "QSplitter::handle:horizontal { width: 3px; }\n"
      "QSplitter::handle:vertical { height: 3px; }\n";

   return qss;
}

void US_Theme::invalidate( void )
{
   applied_signature.clear();
}

void US_Theme::apply( bool force )
{
   if ( qApp == nullptr )
      return;

   // Sample the desktop's scheme before we replace the application palette
   systemScheme();

   const QString signature = QString( "%1|%2|%3|%4" )
      .arg( US_GuiSettings::guiStyle () )
      .arg( US_GuiSettings::fontFamily() )
      .arg( US_GuiSettings::fontSize  () )
      .arg( static_cast< int >( scheme() ) );

   if ( ! force  &&  signature == applied_signature )
      return;

   applied_signature = signature;

   QStyle* style = QStyleFactory::create( US_GuiSettings::guiStyle() );

   if ( style == nullptr )
      style = QStyleFactory::create( defaultStyle() );

   if ( style != nullptr )
      QApplication::setStyle( style );

   // The palette has to follow the style: setStyle() installs the style's
   // own standard palette.
   QApplication::setPalette  ( applicationPalette() );
   QApplication::setFont     ( baseFont() );
   qApp->setStyleSheet       ( styleSheet() );

#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
   if ( ! watcher_installed  &&  QGuiApplication::styleHints() != nullptr )
   {
      watcher_installed = true;

      QObject::connect( QGuiApplication::styleHints(),
                        &QStyleHints::colorSchemeChanged,
                        qApp,
                        []( Qt::ColorScheme )
                        {
                           // Follow the desktop switching between light and
                           // dark while UltraScan is running.
                           US_Theme::invalidate();
                           US_Theme::apply();
                        } );
   }
#endif
}
