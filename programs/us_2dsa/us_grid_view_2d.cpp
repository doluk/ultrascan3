//! \file us_grid_view_2d.cpp
#include <QApplication>

#include "us_grid_view_2d.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_model.h"
#include "us_math2.h"
#include "qwt_picker_machine.h"

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()
#endif

// Constructor:  2DSA grid display widget
US_GridView2D::US_GridView2D( const QList< QVector< US_Solute > >& subgrids,
   const QString& desc, QWidget* p ) : US_WidgetsDialog( p, Qt::WindowFlags() )
{
   dbg_level      = US_Settings::us_debug();

   setObjectName( "US_GridView2D" );
   setAttribute( Qt::WA_DeleteOnClose, true );
   setPalette( US_GuiSettings::frameColor() );
   setFont( QFont( US_GuiSettings::fontFamily(), US_GuiSettings::fontSize() ) );
   setWindowTitle( tr( "2-D Spectrum Analysis Grid" ) );

   QHBoxLayout* main  = new QHBoxLayout( this );
   QGridLayout* left  = new QGridLayout();
   QVBoxLayout* right = new QVBoxLayout();
   main->setSpacing        ( 2 );
   main->setContentsMargins( 2, 2, 2, 2 );

   // Grid information and subgrid controls on the left side
   QLabel* lb_info         = us_banner( tr( "Grid Information" ) );
   QLabel* lb_desc         = us_label(  tr( "Grid Type:" ) );
   QLabel* lb_npoints      = us_label(  tr( "Total Grid Points:" ) );
   QLabel* lb_nsubgrids    = us_label(  tr( "Number of Subgrids:" ) );
   QLabel* lb_subgrid_ctrl = us_banner( tr( "Subgrid Control" ) );
   QLabel* lb_subgrid      = us_label(  tr( "Subgrid:" ) );
   QLabel* lb_npoints_curr = us_label(  tr( "Points in Subgrid:" ) );
   QLabel* lb_hint         = us_label(
      tr( "Click on a grid point to highlight its subgrid." ) );
   lb_hint->setWordWrap( true );

   le_desc         = us_lineedit( "", -1, true );
   le_npoints      = us_lineedit( "", -1, true );
   le_nsubgrids    = us_lineedit( "", -1, true );
   le_npoints_curr = us_lineedit( "", -1, true );

   QGridLayout* lo_highlight =
      us_checkbox( tr( "Highlight Subgrid" ),   ck_highlight, true  );
   QGridLayout* lo_colorall  =
      us_checkbox( tr( "Color All Subgrids" ),  ck_colorall,  false );

   ct_subgrid      = us_counter( 2, 1, 1, 1 );
   ct_subgrid->setSingleStep( 1 );
   QLineEdit* le_sg = ct_subgrid->findChild< QLineEdit* >();
   if ( le_sg )
      le_sg->setAlignment( Qt::AlignCenter );

   QPushButton* pb_help  = us_pushbutton( tr( "Help" ) );
   QPushButton* pb_close = us_pushbutton( tr( "Close" ) );

   int row = 0;
   left->addWidget( lb_info,          row++, 0, 1, 4 );
   left->addWidget( lb_desc,          row,   0, 1, 2 );
   left->addWidget( le_desc,          row++, 2, 1, 2 );
   left->addWidget( lb_npoints,       row,   0, 1, 2 );
   left->addWidget( le_npoints,       row++, 2, 1, 2 );
   left->addWidget( lb_nsubgrids,     row,   0, 1, 2 );
   left->addWidget( le_nsubgrids,     row++, 2, 1, 2 );
   left->addWidget( lb_subgrid_ctrl,  row++, 0, 1, 4 );
   left->addLayout( lo_highlight,     row,   0, 1, 2 );
   left->addLayout( lo_colorall,      row++, 2, 1, 2 );
   left->addWidget( lb_subgrid,       row,   0, 1, 2 );
   left->addWidget( ct_subgrid,       row++, 2, 1, 2 );
   left->addWidget( lb_npoints_curr,  row,   0, 1, 2 );
   left->addWidget( le_npoints_curr,  row++, 2, 1, 2 );
   left->addWidget( lb_hint,          row++, 0, 1, 4 );
   QSpacerItem* spacer = new QSpacerItem( 0, 0, QSizePolicy::Minimum,
                                          QSizePolicy::Expanding );
   left->addItem  ( spacer,           row++, 0, 1, 4 );
   left->addWidget( pb_help,          row,   0, 1, 2 );
   left->addWidget( pb_close,         row++, 2, 1, 2 );

   for ( int ii = 0; ii < 4; ii++ )
      left->setColumnStretch( ii, 1 );

   // Grid plot on the right side
   QBoxLayout* plot_lyt = new US_Plot( data_plot,
                                       tr( "Grid Layout" ),
                                       tr( "" ),
                                       tr( "" ) );
   data_plot->setAutoDelete( true );
   data_plot->setMinimumSize( 640, 480 );
   data_plot->enableAxis( QwtPlot::xBottom, true );
   data_plot->enableAxis( QwtPlot::yLeft,   true );
   data_plot->setCanvasBackground( QBrush( QColor( 32, 32, 32 ) ) );

   color_base      = QColor( 0,    47, 167 );
   color_subgrid   = QColor( 173, 255,  47 );
   hl_curve        = us_curve( data_plot, "SUBGRID" );
   hl_curve->setStyle( QwtPlotCurve::NoCurve );
   hl_curve->setZ( 1 );

   pick            = new QwtPlotPicker( QwtPlot::xBottom, QwtPlot::yLeft,
                                        data_plot->canvas() );
   pick->setStateMachine( new QwtPickerClickPointMachine() );
   pick->setTrackerMode ( QwtPicker::AlwaysOff );

   // Plot controls below the plot
   QRadioButton* rb_x[ 5 ];
   QRadioButton* rb_y[ 5 ];
   QGridLayout*  lo_x[ 5 ];
   QGridLayout*  lo_y[ 5 ];
   x_axis          = new QButtonGroup( this );
   y_axis          = new QButtonGroup( this );

   for ( int ii = ATTR_S; ii <= ATTR_D; ii++ )
   {
      lo_x[ ii ]      = us_radiobutton( attr_symbol( ii ), rb_x[ ii ], false );
      lo_y[ ii ]      = us_radiobutton( attr_symbol( ii ), rb_y[ ii ], false );
      x_axis->addButton( rb_x[ ii ], ii );
      y_axis->addButton( rb_y[ ii ], ii );
   }

   QLabel* lb_plt_cntrl = us_banner( tr( "Plot Control" ) );
   QLabel* lb_x_plot    = us_label( tr( "X-Axis" ) );
   QLabel* lb_y_plot    = us_label( tr( "Y-Axis" ) );
   QLabel* lb_p_size    = us_label( tr( "Point Size" ) );
   lb_x_plot->setAlignment( Qt::AlignCenter );
   lb_y_plot->setAlignment( Qt::AlignCenter );
   lb_p_size->setAlignment( Qt::AlignCenter );

   ct_size         = us_counter( 1, 1, 50, 2 );
   ct_size->setSingleStep( 1 );
   QLineEdit* le_sz = ct_size->findChild< QLineEdit* >();
   if ( le_sz )
      le_sz->setAlignment( Qt::AlignCenter );

   QGridLayout* lyt_r = new QGridLayout();
   lyt_r->setContentsMargins( 0, 0, 0, 0 );
   lyt_r->addWidget( lb_x_plot, 0, 0, 1, 1 );
   lyt_r->addWidget( lb_y_plot, 1, 0, 1, 1 );

   for ( int ii = ATTR_S; ii <= ATTR_D; ii++ )
   {
      lyt_r->addLayout( lo_x[ ii ], 0, ii + 1, 1, 1 );
      lyt_r->addLayout( lo_y[ ii ], 1, ii + 1, 1, 1 );
   }

   lyt_r->addWidget( lb_p_size, 2, 0, 1, 1 );
   lyt_r->addWidget( ct_size,   2, 1, 1, 2 );

   for ( int ii = 0; ii < lyt_r->columnCount(); ii++ )
      lyt_r->setColumnStretch( ii, 1 );

   right->addLayout( plot_lyt );
   right->addWidget( lb_plt_cntrl );
   right->addLayout( lyt_r );

   main->addLayout( left );
   main->addLayout( right );
   main->setStretchFactor( left,  2 );
   main->setStretchFactor( right, 6 );

   rb_x[ ATTR_S ]->setChecked( true );
   rb_y[ ATTR_K ]->setChecked( true );

   connect( x_axis,       &QButtonGroup::idReleased,
            this,         &US_GridView2D::select_axis );
   connect( y_axis,       &QButtonGroup::idReleased,
            this,         &US_GridView2D::select_axis );
   connect( ct_size,      &QwtCounter::valueChanged,
            this,         &US_GridView2D::set_symbol_size );
   connect( ct_subgrid,   &QwtCounter::valueChanged,
            this,         &US_GridView2D::subgrid_changed );
   connect( ck_highlight, &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( ck_colorall,  &QCheckBox::toggled,
            this,         &US_GridView2D::plot_points );
   connect( pick,         QOverload< const QPointF& >::of( &QwtPlotPicker::selected ),
            this,         &US_GridView2D::point_clicked );
   connect( pb_help,      &QPushButton::clicked,
            this,         &US_GridView2D::help );
   connect( pb_close,     &QPushButton::clicked,
            this,         &US_GridView2D::close );

   set_grid( subgrids, desc );
}

// Replace the grid to display
void US_GridView2D::set_grid( const QList< QVector< US_Solute > >& subgrids,
                              const QString& desc )
{
   grid.clear();
   int  npoints   = 0;
   bool cnst_k    = true;
   bool cnst_v    = true;
   double k0      = 0.0;
   double v0      = 0.0;

   for ( int ii = 0; ii < subgrids.size(); ii++ )
   {
      QVector< GridPt > gpts;

      for ( int jj = 0; jj < subgrids[ ii ].size(); jj++ )
      {
         const US_Solute& sol = subgrids[ ii ][ jj ];
         gpts << make_point( sol );

         if ( npoints == 0 )
         {
            k0             = sol.k;
            v0             = sol.v;
         }
         cnst_k         = cnst_k  &&  ( sol.k == k0 );
         cnst_v         = cnst_v  &&  ( sol.v == v0 );
         npoints++;
      }

      grid << gpts;
   }

   // For a varying-vbar grid, default the Y axis to vbar
   if ( npoints > 0  &&  cnst_k  &&  ! cnst_v )
      y_axis->button( ATTR_V )->setChecked( true );
   else if ( y_axis->checkedId() == ATTR_V  &&  cnst_v )
      y_axis->button( ATTR_K )->setChecked( true );

   int nsubg       = grid.size();
   le_desc     ->setText( desc );
   le_npoints  ->setText( QString::number( npoints ) );
   le_nsubgrids->setText( QString::number( nsubg ) );

   {
      QSignalBlocker sblock( ct_subgrid );
      ct_subgrid->setRange( 1, qMax( 1, nsubg ) );
      ct_subgrid->setValue( qMin( (int)ct_subgrid->value(), qMax( 1, nsubg ) ) );
   }
DbgLv(1) << "GV2D:SG: npoints nsubg" << npoints << nsubg << desc;

   plot_points();
}

// Compute all attribute values for a solute point
US_GridView2D::GridPt US_GridView2D::make_point( const US_Solute& sol )
{
   GridPt gpt;
   US_Model::SimulationComponent comp;
   comp.s         = sol.s;
   comp.f_f0      = sol.k;
   comp.vbar20    = sol.v;
   comp.mw        = 0.0;
   comp.D         = 0.0;
   comp.f         = 0.0;

   if ( ! US_Model::calc_coefficients( comp ) )
   {  // Inconsistent buoyancy:  derived values are undefined
      comp.mw        = 0.0;
      comp.D         = 0.0;
   }

   gpt.vals[ ATTR_S ] = sol.s * 1.0e+13;
   gpt.vals[ ATTR_K ] = sol.k;
   gpt.vals[ ATTR_M ] = comp.mw / 1000.0;
   gpt.vals[ ATTR_V ] = comp.vbar20;
   gpt.vals[ ATTR_D ] = comp.D;
   return gpt;
}

// Return the axis title for an attribute
QString US_GridView2D::attr_title( int attr )
{
   switch ( attr )
   {
      default:
      case ATTR_S:
         return tr( "Sedimentation Coefficient ( 20,W ) [ Sv ]" );
      case ATTR_K:
         return tr( "Frictional Ratio" );
      case ATTR_M:
         return tr( "Molecular Weight [ kDa ]" );
      case ATTR_V:
         return tr( "Partial Specific Volume [ mL / g ]" );
      case ATTR_D:
         return tr( "<p>Diffusion Coefficient ( 20,W ) "
                    "[ cm<sup>2</sup> s<sup>-1</sup> ]</p>" );
   }
}

// Return the short symbol for an attribute
QString US_GridView2D::attr_symbol( int attr )
{
   switch ( attr )
   {
      default:
      case ATTR_S:  return QString( "s" );
      case ATTR_K:  return QString( "f/f0" );
      case ATTR_M:  return QString( "MW" );
      case ATTR_V:  return QString( "vbar" );
      case ATTR_D:  return QString( "D" );
   }
}

// Return a distinct color for a subgrid
QColor US_GridView2D::subgrid_color( int sgx )
{
   // Golden-angle hue steps give well separated neighbor colors
   int hue        = ( sgx * 137 ) % 360;
   int sat        = ( ( sgx / 360 ) % 2 == 0 ) ? 200 : 140;
   return QColor::fromHsv( hue, sat, 255 );
}

// Plot all grid points
void US_GridView2D::plot_points()
{
   for ( int ii = 0; ii < point_curves.size(); ii++ )
   {
      point_curves[ ii ]->detach();
      delete point_curves[ ii ];
   }
   point_curves.clear();

   int xattr       = x_axis->checkedId();
   int yattr       = y_axis->checkedId();
   data_plot->setAxisTitle( QwtPlot::xBottom, attr_title( xattr ) );
   data_plot->setAxisTitle( QwtPlot::yLeft,   attr_title( yattr ) );

   double px1      =  1e99;
   double px2      = -1e99;
   double py1      =  1e99;
   double py2      = -1e99;
   int    ss       = (int)ct_size->value();
   bool   colorall = ck_colorall->isChecked();

   for ( int ii = 0; ii < grid.size(); ii++ )
   {
      int np          = grid[ ii ].size();
      QVector< double > xarr( np );
      QVector< double > yarr( np );

      for ( int jj = 0; jj < np; jj++ )
      {
         double xx       = grid[ ii ][ jj ].vals[ xattr ];
         double yy       = grid[ ii ][ jj ].vals[ yattr ];
         xarr[ jj ]      = xx;
         yarr[ jj ]      = yy;
         px1             = qMin( px1, xx );
         px2             = qMax( px2, xx );
         py1             = qMin( py1, yy );
         py2             = qMax( py2, yy );
      }

      QColor color    = colorall ? subgrid_color( ii ) : color_base;
      QwtSymbol* symbol = new QwtSymbol( QwtSymbol::Ellipse, QBrush( color ),
                                         QPen( color, 2 ), QSize( ss, ss ) );
      QwtPlotCurve* curve = us_curve( data_plot,
                                      QString( "GRID_%1" ).arg( ii ) );
      curve->setSymbol ( symbol );
      curve->setStyle  ( QwtPlotCurve::NoCurve );
      curve->setSamples( xarr.data(), yarr.data(), np );
      curve->setZ      ( 0 );
      point_curves << curve;
   }

   if ( px1 > px2 )
   {  // No points
      px1             = 0.0;
      px2             = 1.0;
      py1             = 0.0;
      py2             = 1.0;
   }

   // Pad the ranges and insure they are non-empty
   const double fac = 0.05;
   double dx       = ( px2 - px1 ) * fac;
   double dy       = ( py2 - py1 ) * fac;
   dx              = ( dx > 0.0 ) ? dx
                   : ( ( px1 != 0.0 ) ? qAbs( px1 ) * fac : 1.0 );
   dy              = ( dy > 0.0 ) ? dy
                   : ( ( py1 != 0.0 ) ? qAbs( py1 ) * fac : 1.0 );

   data_plot->setAxisScale( QwtPlot::xBottom, px1 - dx, px2 + dx );
   data_plot->setAxisScale( QwtPlot::yLeft,   py1 - dy, py2 + dy );

   plot_subgrid();
}

// Plot the highlighted subgrid
void US_GridView2D::plot_subgrid()
{
   int sgx         = (int)ct_subgrid->value() - 1;
   bool highlight  = ck_highlight->isChecked()  &&
                     sgx >= 0  &&  sgx < grid.size();
   ct_subgrid     ->setEnabled( ck_highlight->isChecked() );

   if ( ! highlight )
   {
      hl_curve->setSamples( QVector< QPointF >() );
      le_npoints_curr->setText( sgx >= 0  &&  sgx < grid.size()
                                ? QString::number( grid[ sgx ].size() )
                                : QString() );
      data_plot->replot();
      return;
   }

   int xattr       = x_axis->checkedId();
   int yattr       = y_axis->checkedId();
   int np          = grid[ sgx ].size();
   QVector< double > xarr( np );
   QVector< double > yarr( np );

   for ( int jj = 0; jj < np; jj++ )
   {
      xarr[ jj ]      = grid[ sgx ][ jj ].vals[ xattr ];
      yarr[ jj ]      = grid[ sgx ][ jj ].vals[ yattr ];
   }

   // With all subgrids colored, highlight in white for contrast
   QColor color    = ck_colorall->isChecked() ? QColor( Qt::white )
                                              : color_subgrid;
   int    ss       = (int)ct_size->value() + 2;
   QwtSymbol* symbol = new QwtSymbol( QwtSymbol::Ellipse, QBrush( color ),
                                      QPen( color, 2 ), QSize( ss, ss ) );
   hl_curve->setTitle  ( QString( "SUBGRID_%1" ).arg( sgx ) );
   hl_curve->setSymbol ( symbol );
   hl_curve->setSamples( xarr.data(), yarr.data(), np );

   le_npoints_curr->setText( QString::number( np ) );
   data_plot->replot();
}

// Change the plot symbol size
void US_GridView2D::set_symbol_size( double )
{
   plot_points();
}

// Change the X or Y axis attribute
void US_GridView2D::select_axis( int )
{
   plot_points();
}

// Change the highlighted subgrid
void US_GridView2D::subgrid_changed( double )
{
   plot_subgrid();
}

// Highlight the subgrid of the grid point nearest a mouse click
void US_GridView2D::point_clicked( const QPointF& pos )
{
   const double maxdist = 10.0;       // Maximum pixel distance
   int    xattr    = x_axis->checkedId();
   int    yattr    = y_axis->checkedId();
   double cx       = data_plot->transform( QwtPlot::xBottom, pos.x() );
   double cy       = data_plot->transform( QwtPlot::yLeft,   pos.y() );
   double mindist  = 1e99;
   int    minsgx   = -1;

   for ( int ii = 0; ii < grid.size(); ii++ )
   {
      for ( int jj = 0; jj < grid[ ii ].size(); jj++ )
      {
         double px       = data_plot->transform( QwtPlot::xBottom,
                                         grid[ ii ][ jj ].vals[ xattr ] );
         double py       = data_plot->transform( QwtPlot::yLeft,
                                         grid[ ii ][ jj ].vals[ yattr ] );
         double dist     = sq( px - cx ) + sq( py - cy );

         if ( dist < mindist )
         {
            mindist         = dist;
            minsgx          = ii;
         }
      }
   }

   if ( minsgx < 0  ||  mindist > sq( maxdist ) )
      return;

   {
      QSignalBlocker sblock( ct_subgrid );
      ct_subgrid->setValue( minsgx + 1 );
   }

   if ( ck_highlight->isChecked() )
      plot_subgrid();
   else
      ck_highlight->setChecked( true );      // Triggers plot_subgrid
}
