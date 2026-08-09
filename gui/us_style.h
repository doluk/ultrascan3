//! \file us_style.h
#ifndef US_STYLE_H
#define US_STYLE_H

#include <QtWidgets>

#include "us_extern.h"

//! \brief The UltraScan widget style: modern shapes, per-widget palettes.
/*!  A Qt style sheet cannot be used to reshape the widgets that UltraScan
     lets the user recolor.  The moment a rule touches a widget's box, Qt
     paints that box from the rule, and a rule's <tt>palette()</tt> function
     always resolves against the *application* palette - never against the
     palette that was set on the widget.  UltraScan sets a palette on nearly
     every widget it creates (and on individual widgets for read-only fields,
     invalid input, and so on), so all of those colors would be lost.

     A QStyle has no such limitation: it is handed the widget's own palette in
     QStyleOption::palette.  US_Style therefore draws the rounded, flat
     UltraScan shapes itself and takes every color from that palette, which
     keeps the color configuration panel - and any per-widget palette a
     program sets - fully in charge.

     Everything this class does not override falls through to the base style
     (Fusion by default, or whichever style the user selected).
*/
class US_GUI_EXTERN US_Style : public QProxyStyle
{
   Q_OBJECT

   public:
      //! \brief Wrap a base style
      //! \param base The style to fall back to.  US_Style takes ownership.
      explicit US_Style( QStyle* base );

      void drawPrimitive     ( PrimitiveElement, const QStyleOption*,
                               QPainter*, const QWidget* = nullptr )
                               const override;

      void drawControl       ( ControlElement, const QStyleOption*,
                               QPainter*, const QWidget* = nullptr )
                               const override;

      void drawComplexControl( ComplexControl, const QStyleOptionComplex*,
                               QPainter*, const QWidget* = nullptr )
                               const override;

      int  pixelMetric       ( PixelMetric, const QStyleOption* = nullptr,
                               const QWidget* = nullptr ) const override;

      int  styleHint         ( StyleHint, const QStyleOption* = nullptr,
                               const QWidget* = nullptr,
                               QStyleHintReturn* = nullptr ) const override;
};

#endif
