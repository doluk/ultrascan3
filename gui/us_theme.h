//! \file us_theme.h
#ifndef US_THEME_H
#define US_THEME_H

#include <QtWidgets>
#include <QtGui>

#include "us_extern.h"

//! \brief The set of design tokens that make up one UltraScan color scheme.
/*!  A token is a named, semantic color.  Everything that UltraScan paints -
     the default widget palettes in US_GuiSettings as well as the application
     wide style sheet - is derived from these values, so that changing the look
     of all UltraScan programs is a matter of editing a single table.
*/
struct US_GUI_EXTERN US_ThemeTokens
{
   // Surfaces and text
   QColor window;         //!< Window / dialog background
   QColor windowAlt;      //!< Slightly raised surface on top of the window
   QColor windowText;     //!< Default foreground on \ref window
   QColor mutedText;      //!< De-emphasized foreground
   QColor disabledText;   //!< Foreground of disabled widgets

   // Data entry
   QColor base;           //!< Background of editable fields and lists
   QColor baseAlt;        //!< Alternating row background
   QColor baseReadOnly;   //!< Background of read-only fields
   QColor text;           //!< Foreground on \ref base

   // Buttons
   QColor button;         //!< Push button background
   QColor buttonHover;    //!< Push button background, mouse over
   QColor buttonPressed;  //!< Push button background, pressed
   QColor buttonText;     //!< Push button foreground

   // Lines
   QColor border;         //!< Ordinary 1px separators and widget outlines
   QColor borderStrong;   //!< Emphasized separators
   QColor shadow;         //!< Darkest line color

   // Accent
   QColor accent;         //!< Selection / focus / primary accent fill
   QColor accentText;     //!< Foreground on \ref accent
   QColor accentBright;   //!< Accent variant that stays legible as a line
   QColor attention;      //!< "Bright text" - warnings and error emphasis

   // Section banners
   QColor bannerBg;       //!< Section header background
   QColor bannerText;     //!< Section header foreground

   // Tool tips
   QColor tipBg;          //!< Tool tip background
   QColor tipText;        //!< Tool tip foreground

   // Plots
   QColor plotBg;         //!< Plot canvas background
   QColor plotMajGrid;    //!< Major grid lines
   QColor plotMinGrid;    //!< Minor grid lines
   QColor plotPicker;     //!< Picker / tracker cross hairs
   QColor plotCurve;      //!< Default curve color

   // LCD panels
   QColor lcdBg;          //!< LCD background
   QColor lcdText;        //!< LCD digits
   QColor lcdHi1;         //!< LCD highlight 1
   QColor lcdHi2;         //!< LCD highlight 2
};

//! \brief The central look-and-feel definition for all UltraScan programs.
/*!  US_Theme owns three things:
     - the token table for the light and the dark color scheme,
     - the decision which of the two is currently in effect,
     - the application wide QStyle, palette, font and style sheet.

     The style sheet deliberately stays away from every widget the user can
     recolor in the "Color Configuration" panel of us_config.  A style sheet
     rule that touches a widget's box takes the painting of that widget away
     from the style, and Qt then ignores the QPalette that was set on the
     widget - inside a rule, <tt>palette()</tt> resolves against the
     application palette, not the widget's own.  Labels, banners, push
     buttons, edit fields, item views, LCDs and plots are therefore styled
     through their palettes only; the style sheet modernizes the chrome that
     has no palette of its own (menus, tabs, scroll bars, indicators, ...).
*/
class US_GUI_EXTERN US_Theme
{
   public:

      //! \brief The two color schemes UltraScan ships with
      enum Scheme
      {
         Light,   //!< Light scheme
         Dark     //!< Dark scheme
      };

      //! \brief Name of the dynamic property that tags a section banner
      /*!  US_Widgets::us_banner() sets it to "banner".  UltraScan's own style
           sheet does not use it - it is a hook for site specific style sheets
           that want to single out section headers.
      */
      static const char* bannerProperty() { return "usRole"; }

      //! \brief The color scheme currently in effect
      static Scheme  scheme          ( void );

      //! \brief Convenience test for US_Theme::scheme() == Dark
      static bool    isDark          ( void );

      //! \brief The token table of the scheme currently in effect
      static US_ThemeTokens tokens   ( void );

      //! \brief The token table of a specific scheme
      static US_ThemeTokens tokens   ( Scheme );

      //! \brief User preference for the color scheme: "auto", "light", "dark"
      static QString schemeSetting    ( void );

      //! \brief Store the user preference for the color scheme
      static void set_schemeSetting   ( const QString& );

      //! \brief The color scheme the desktop environment asks for
      static Scheme  systemScheme    ( void );

      //! \brief The widget style UltraScan uses unless the user picks another
      static QString defaultStyle    ( void );

      //! \brief The application font built from the US_GuiSettings values
      static QFont   baseFont        ( void );

      //! \brief The application wide palette (menus, dialogs, tool tips, ...)
      static QPalette applicationPalette( void );

      //! \brief The application wide style sheet (shape only, no fixed colors)
      static QString styleSheet      ( void );

      //! \brief Apply style, palette, font and style sheet to the application
      //! \param force Re-apply even when nothing appears to have changed
      static void    apply           ( bool force = false );

      //! \brief Forget what was applied last, so the next apply() rebuilds all
      static void    invalidate      ( void );
};

#endif
