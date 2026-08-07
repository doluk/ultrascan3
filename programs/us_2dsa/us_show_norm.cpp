//! \file us_show_norm.cpp

#include "us_2dsa.h"
#include "us_analysis_control_2d.h"
#include "us_adv_analysis_2d.h"
#include "us_settings.h"
#include "us_passwd.h"
#include "us_db2.h"
#include "us_gui_settings.h"
#include "us_memory.h"
#include "us_spectrodata.h"
#include "us_model.h"
#include "us_model_loader.h"
#include "us_show_norm.h"
#include "us_select_runs.h"
#include "us_model.h"
#include "us_license_t.h"
#include "us_license.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_gui_util.h"
#include "us_math2.h"
#include "us_matrix.h"
#include "us_sleep.h"
#include "us_passwd.h"
#include "us_report.h"
#include "us_constants.h"
#include <qwt_legend.h>

#define setPBMaximum(a)   setRange(1,a)

// Function to compare solute points for sorting
bool distro_lessthan( const S_Solute &solu1, const S_Solute &solu2 )
{  // TRUE iff  (s1<s2) || (s1==s2 && k1<k2)
   return ( solu1.s < solu2.s ) ||
          ( ( solu1.s == solu2.s ) && ( solu1.k < solu2.k ) );
}

//------------------------------------------------------------------------
// US_Pseudo3D_Combine class constructor

US_show_norm::US_show_norm(  US_Model* a_model, bool a_cnst_vbar, QWidget* p )
   : US_WidgetsDialog( p, Qt::WindowFlags() )

{
   // set up the GUI
   cnst_vbar      = a_cnst_vbar;
   model          = a_model;      // must be set before any set_limits() call
   parentw        = p;
   dbg_level      = US_Settings::us_debug();
   colormap       = NULL;

   setWindowTitle( tr( "Plot norms on grids" ) );
   setPalette( US_GuiSettings::frameColor() );

   // primary layouts
   QHBoxLayout* main = new QHBoxLayout( this );
   QVBoxLayout* left = new QVBoxLayout();
   QGridLayout* spec = new QGridLayout();
   main->setSpacing        ( 2 );
   main->setContentsMargins( 2, 2, 2, 2 );
   left->setSpacing        ( 0 );
   left->setContentsMargins( 0, 1, 0, 1 );
   spec->setSpacing        ( 1 );
   spec->setContentsMargins( 0, 0, 0, 0 );

   int s_row = 0;
   dbg_level = US_Settings::us_debug();

   // Top banner
   QLabel* lb_info1      = us_banner( tr( "Show norms on grids" ) );

   // Series of rows: most of them label on left, counter/box on right
   QLabel* lb_resolu     = us_label( tr( "Norm Resolution:" ) );
   lb_resolu->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_resolu     = us_counter( 3, 0.0, 100.0, 90.0 );
   ct_resolu->setSingleStep( 1 );
   connect( ct_resolu, SIGNAL( valueChanged( double ) ),
            this,      SLOT( update_resolu( double ) ) );

   QLabel* lb_xreso      = us_label( tr( "X Resolution:" ) );
   lb_xreso->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_xreso      = us_counter( 3, 10.0, 1000.0, 0.0 );
   ct_xreso->setSingleStep( 1 );
   connect( ct_xreso,  SIGNAL( valueChanged( double ) ),
            this,      SLOT( update_xreso( double ) ) );

   QLabel* lb_yreso      = us_label( tr( "Y Resolution:" ) );
   lb_yreso->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_yreso      = us_counter( 3, 10.0, 1000.0, 0.0 );
   ct_yreso->setSingleStep( 1 );
   connect( ct_yreso,  SIGNAL( valueChanged( double ) ),
            this,      SLOT( update_yreso( double ) ) );

   QLabel* lb_zfloor     = us_label( tr( "Z Visibility Percent:" ) );
   lb_zfloor->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_zfloor     = us_counter( 3, 50.0, 150.0, 1.0 );
   ct_zfloor->setSingleStep( 1 );
   connect( ct_zfloor, SIGNAL( valueChanged( double ) ),
            this,      SLOT( update_zfloor( double ) ) );

   us_checkbox( tr( "Autoscale X and Y" ), ck_autosxy, true );
   connect( ck_autosxy, SIGNAL( clicked() ),
            this,       SLOT( select_autosxy() ) );

   // The Z range is always taken from the data:  there is no manual Z entry
   //  to fall back on, so this is shown checked and disabled rather than
   //  offering a choice that cannot be honored.
   us_checkbox( tr( "Autoscale Z" ), ck_autoscz, true );
   ck_autoscz->setEnabled( false );
   ck_autoscz->setToolTip( tr( "The Z range is always taken from the data" ) );

   // Not yet implemented for the norm display:  shown disabled rather than
   //  connected to a slot that does not exist.
   us_checkbox( tr( "Continuous Loop" ), ck_conloop, false );
   ck_conloop->setEnabled( false );
   ck_conloop->setToolTip( tr( "Not implemented for norm grids" ) );

   us_checkbox( tr( "Z as Percentage" ), ck_zpcent,  false );

   us_checkbox( tr( "Save Plot(s)"    ), ck_savepl,  false );
   us_checkbox( tr( "Local Save Only" ), ck_locsave, true  );

   lb_plt_kmin   = us_label( tr( "Plot Limit f/f0 Minimum:" ) );
   lb_plt_kmin->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_plt_kmin   = us_counter( 3, 0.5, 50.0, 1.0 );
   ct_plt_kmin->setSingleStep( 1 );
   connect( ct_plt_kmin, SIGNAL( valueChanged( double ) ),
            this,        SLOT( update_plot_kmin( double ) ) );

   lb_plt_kmax   = us_label( tr( "Plot Limit f/f0 Maximum:" ) );
   lb_plt_kmax->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_plt_kmax   = us_counter( 3, 1.0, 50.0, 4.0 );
   ct_plt_kmax->setSingleStep( 1 );
   connect( ct_plt_kmax, SIGNAL( valueChanged( double ) ),
            this,        SLOT( update_plot_kmax( double ) ) );

   lb_plt_smin   = us_label( tr( "Plot Limit s Minimum:" ) );
   lb_plt_smin->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_plt_smin   = us_counter( 3, -10.0, 10000.0, 1.0 );
   ct_plt_smin->setSingleStep( 1 );
   connect( ct_plt_smin, SIGNAL( valueChanged( double ) ),
            this,        SLOT( update_plot_smin( double ) ) );

   lb_plt_smax   = us_label( tr( "Plot Limit s Maximum:" ) );
   lb_plt_smax->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_plt_smax   = us_counter( 3, 0.0, 10000.0, 10.0 );
   ct_plt_smax->setSingleStep( 1 );
   connect( ct_plt_smax, SIGNAL( valueChanged( double ) ),
            this,        SLOT( update_plot_smax( double ) ) );

   QLabel* lb_curr_distr = us_label( tr( "Current Distro:" ) );
   lb_curr_distr->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   ct_curr_distr = us_counter( 3, 0.0, 10.0, 0.0 );
   ct_curr_distr->setSingleStep( 1 );
   // Only a single distribution is ever shown here, so this selector has
   //  nothing to select:  shown disabled rather than connected to a slot
   //  that does not exist.
   ct_curr_distr->setEnabled( false );
   ct_curr_distr->setToolTip( tr( "Only one norm distribution is loaded" ) );

   te_distr_info = us_textedit();
   te_distr_info->setText    ( tr( "Run:  runID.triple (method)\n" )
            + tr( "    analysisID" ) );
   us_setReadOnly( te_distr_info, true );

   le_cmap_name  = us_lineedit(
         tr( "Default Color Map: w-cyan-magenta-red-black" ), -1, true );
   te_distr_info->setMaximumHeight( le_cmap_name->height() * 2 );

           plot_x      = 0;
           plot_y      = 1;
   QLabel* lb_x_axis   = us_label( tr( "Plot X:" ) );
   QLabel* lb_y_axis   = us_label( tr( "Plot Y:" ) );
           bg_x_axis   = new QButtonGroup( this );
           bg_y_axis   = new QButtonGroup( this );
   QGridLayout*  gl_x_s    = us_radiobutton( tr( "s"   ), rb_x_s,    true  );
   QGridLayout*  gl_x_ff0  = us_radiobutton( tr( "ff0" ), rb_x_ff0,  false );
   QGridLayout*  gl_x_mw   = us_radiobutton( tr( "mw"  ), rb_x_mw,   false );
   QGridLayout*  gl_x_vbar = us_radiobutton( tr( "vbar"), rb_x_vbar, false );
   QGridLayout*  gl_x_D    = us_radiobutton( tr( "D"   ), rb_x_D,    false );
   QGridLayout*  gl_x_f    = us_radiobutton( tr( "f"   ), rb_x_f,    false );
   QGridLayout*  gl_y_s    = us_radiobutton( tr( "s"   ), rb_y_s,    false );
   QGridLayout*  gl_y_ff0  = us_radiobutton( tr( "ff0" ), rb_y_ff0,  true  );
   QGridLayout*  gl_y_mw   = us_radiobutton( tr( "mw"  ), rb_y_mw,   false );
   QGridLayout*  gl_y_vbar = us_radiobutton( tr( "vbar"), rb_y_vbar, false );
   QGridLayout*  gl_y_D    = us_radiobutton( tr( "D"   ), rb_y_D,    false );
   QGridLayout*  gl_y_f    = us_radiobutton( tr( "f"   ), rb_y_f,    false );
   bg_x_axis->addButton( rb_x_s,    ATTR_S );
   bg_x_axis->addButton( rb_x_ff0,  ATTR_K );
   bg_x_axis->addButton( rb_x_mw,   ATTR_W );
   bg_x_axis->addButton( rb_x_vbar, ATTR_V );
   bg_x_axis->addButton( rb_x_D,    ATTR_D );
   bg_x_axis->addButton( rb_x_f,    ATTR_F );
   bg_y_axis->addButton( rb_y_s,    ATTR_S );
   bg_y_axis->addButton( rb_y_ff0,  ATTR_K );
   bg_y_axis->addButton( rb_y_mw,   ATTR_W );
   bg_y_axis->addButton( rb_y_vbar, ATTR_V );
   bg_y_axis->addButton( rb_y_D,    ATTR_D );
   bg_y_axis->addButton( rb_y_f,    ATTR_F );
   rb_x_s   ->setChecked( true  );
   rb_y_s   ->setEnabled( false );
   rb_y_ff0 ->setChecked( true  );
   rb_x_ff0 ->setEnabled( false );
   rb_x_s   ->setToolTip( tr( "Set X axis to Sedimentation Coefficient" ) );
   rb_x_ff0 ->setToolTip( tr( "Set X axis to Frictional Ratio"          ) );
   rb_x_mw  ->setToolTip( tr( "Set X axis to Molecular Weight"          ) );
   rb_x_vbar->setToolTip( tr( "Set X axis to Partial Specific Volume"   ) );
   rb_x_D   ->setToolTip( tr( "Set X axis to Diffusion Coefficient"     ) );
   rb_x_f   ->setToolTip( tr( "Set X axis to Frictional Coefficient"    ) );
   rb_y_s   ->setToolTip( tr( "Set Y axis to Sedimentation Coefficient" ) );
   rb_y_ff0 ->setToolTip( tr( "Set Y axis to Frictional Ratio"          ) );
   rb_y_mw  ->setToolTip( tr( "Set Y axis to Molecular Weight"          ) );
   rb_y_vbar->setToolTip( tr( "Set Y axis to Partial Specific Volume"   ) );
   rb_y_D   ->setToolTip( tr( "Set Y axis to Diffusion Coefficient"     ) );
   rb_y_f   ->setToolTip( tr( "Set Y axis to Frictional Coefficient"    ) );
   connect( bg_x_axis, &QButtonGroup::idReleased, this, &US_show_norm::select_x_axis );
   connect( bg_y_axis, &QButtonGroup::idReleased, this, &US_show_norm::select_y_axis );

   pb_refresh    = us_pushbutton( tr( "Refresh" ) );
   pb_refresh->setEnabled(  true );
   connect( pb_refresh, SIGNAL( clicked() ),
            this,       SLOT( plot_data() ) );

   pb_reset      = us_pushbutton( tr( "Reset" ) );
   pb_reset->setEnabled( true );
   connect( pb_reset,   SIGNAL( clicked() ),
            this,       SLOT( reset() ) );

   pb_ldcolor    = us_pushbutton( tr( "Load Color File" ) );
   pb_ldcolor->setEnabled( true );
   connect( pb_ldcolor, SIGNAL( clicked() ),
            this,       SLOT( load_color() ) );

   pb_close      = us_pushbutton( tr( "Close" ) );
   pb_close->setEnabled( true );
   connect( pb_close,   SIGNAL( clicked() ),
            this,       SLOT( close() ) );

   QFontMetrics fm( ct_plt_smax->font() );
   ct_plt_smax->adjustSize();
   ct_plt_smax->setMinimumWidth( ct_plt_smax->width() + fm.horizontalAdvance( "ABC" ) );

   // Order plot components on the left side
   spec->addWidget( lb_info1,      s_row++, 0, 1, 8 );
   spec->addWidget( lb_resolu,     s_row,   0, 1, 4 );
   spec->addWidget( ct_resolu,     s_row++, 4, 1, 4 );
   spec->addWidget( lb_xreso,      s_row,   0, 1, 4 );
   spec->addWidget( ct_xreso,      s_row++, 4, 1, 4 );
   spec->addWidget( lb_yreso,      s_row,   0, 1, 4 );
   spec->addWidget( ct_yreso,      s_row++, 4, 1, 4 );
   spec->addWidget( lb_zfloor,     s_row,   0, 1, 4 );
   spec->addWidget( ct_zfloor,     s_row++, 4, 1, 4 );
   spec->addWidget( ck_autosxy,    s_row,   0, 1, 4 );
   spec->addWidget( ck_autoscz,    s_row++, 4, 1, 4 );
   spec->addWidget( ck_conloop,    s_row,   0, 1, 4 );
   spec->addWidget( ck_zpcent,     s_row++, 4, 1, 4 );
   spec->addWidget( ck_savepl,     s_row,   0, 1, 4 );
   spec->addWidget( ck_locsave,    s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_kmin,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_kmin,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_kmax,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_kmax,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_smin,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_smin,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_smax,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_smax,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_curr_distr, s_row,   0, 1, 4 );
   spec->addWidget( ct_curr_distr, s_row++, 4, 1, 4 );
   spec->addWidget( te_distr_info, s_row,   0, 2, 8 ); s_row += 2;
   spec->addWidget( le_cmap_name,  s_row++, 0, 1, 8 );
   spec->addWidget( lb_x_axis,     s_row,   0, 1, 2 );
   spec->addLayout( gl_x_s,        s_row,   2, 1, 1 );
   spec->addLayout( gl_x_ff0,      s_row,   3, 1, 1 );
   spec->addLayout( gl_x_mw,       s_row,   4, 1, 1 );
   spec->addLayout( gl_x_vbar,     s_row,   5, 1, 1 );
   spec->addLayout( gl_x_D,        s_row,   6, 1, 1 );
   spec->addLayout( gl_x_f,        s_row++, 7, 1, 1 );
   spec->addWidget( lb_y_axis,     s_row,   0, 1, 2 );
   spec->addLayout( gl_y_s,        s_row,   2, 1, 1 );
   spec->addLayout( gl_y_ff0,      s_row,   3, 1, 1 );
   spec->addLayout( gl_y_mw,       s_row,   4, 1, 1 );
   spec->addLayout( gl_y_vbar,     s_row,   5, 1, 1 );
   spec->addLayout( gl_y_D,        s_row,   6, 1, 1 );
   spec->addLayout( gl_y_f,        s_row++, 7, 1, 1 );
   spec->addWidget( pb_refresh,    s_row++, 0, 1, 8 );
   spec->addWidget( pb_ldcolor,    s_row++, 0, 1, 8 );
   spec->addWidget( pb_reset,      s_row++, 0, 1, 8 );
   spec->addWidget( pb_close,      s_row++, 0, 1, 8 );

   // Set up plot component window on right side
   xa_title    = anno_title( ATTR_S );
   ya_title    = anno_title( ATTR_K );
   QBoxLayout* plot = new US_Plot( data_plot, 
      tr( "Norms on grids" ), xa_title, ya_title );

   data_plot->setMinimumSize( 600, 600 );

   data_plot->enableAxis( QwtPlot::xBottom, true );
   data_plot->enableAxis( QwtPlot::yLeft,   true );
   data_plot->enableAxis( QwtPlot::yRight,  true );
   data_plot->setAxisScale( QwtPlot::xBottom, 1.0, 40.0 );
   data_plot->setAxisScale( QwtPlot::yLeft,   1.0,  4.0 );
   data_plot->setAxisScale( QwtPlot::yRight,  0.0,  0.2 );
   data_plot->setCanvasBackground( Qt::white );
   QwtText zTitle( "Norms of column vector" );
   zTitle.setFont( QFont( US_GuiSettings::fontFamily(),
      US_GuiSettings::fontSize(), QFont::Bold ) );
   data_plot->setAxisTitle( QwtPlot::yRight, zTitle );

   pick = new US_PlotPicker( data_plot );
   pick->setRubberBand( QwtPicker::RectRubberBand );

   // put layouts together for overall layout
   left->addLayout( spec );
   left->addStretch();

   main->addLayout( left );
   main->addLayout( plot );
   main->setStretchFactor( left, 3 );
   main->setStretchFactor( plot, 5 );

   mfilter    = "";
   plt_zmin   = -1e+50;
   plt_zmax   = 1e+12;
   runsel     = true;
   latest     = true;
   reset() ;

   build_xy_distro();

   // Keep the Y-axis selector consistent with the attribute actually
   //  plotted:  vbar for a constant-f/f0 grid, f/f0 otherwise.
   if ( plot_y == ATTR_V )
      rb_y_vbar->setChecked( true );
   else
      rb_y_ff0 ->setChecked( true );

   // Apply the matching axis titles, counter labels and ranges, derive the
   //  plot limits and draw.  Previously the Y labels and limits were left
   //  describing f/f0 even when vbar was the attribute being plotted.
   select_y_axis( plot_y );
}

US_show_norm::~US_show_norm()
{
   delete colormap;
   colormap   = NULL;
}

void US_show_norm::reset( void )
{
   dataPlotClear( data_plot );
   data_plot->replot();
 
   need_save  = false;

   plot_x     = ATTR_S;
   // Y follows the attribute that actually varies over the grid, so that
   //  Reset cannot leave the axis labelled f/f0 while vbar is plotted.
   plot_y     = cnst_vbar ? ATTR_K : ATTR_V;
   resolu     = 90.0;
   ct_resolu->setRange( 1.0, 100.0 );
   ct_resolu->setSingleStep( 1.0 );
   ct_resolu->setValue( resolu );  

   xreso      = 300.0;
   yreso      = 300.0;
   ct_xreso->setRange( 10.0, 1000.0 );
   ct_xreso->setSingleStep( 1.0 );
   ct_xreso->setValue( (double)xreso );
   ct_yreso->setRange( 10.0, 1000.0 );
   ct_yreso->setSingleStep( 1.0 );
   ct_yreso->setValue( (double)yreso );

   zfloor     = 100.0;
   ct_zfloor->setRange( 50.0, 150.0 );
   ct_zfloor->setSingleStep( 1.0 );
   ct_zfloor->setValue( (double)zfloor );

   auto_sxy   = true;
   ck_autosxy->setChecked( auto_sxy );
   auto_scz   = true;
   ck_autoscz->setChecked( auto_scz );
   cont_loop  = false;
   ck_conloop->setChecked( cont_loop );
   ck_savepl ->setChecked( false     );
   ck_locsave->setChecked( true      );

   plt_kmin   = 0.8;
   plt_kmax   = 4.2;
   ct_plt_kmin->setRange( 0.0, 50.0 );
   ct_plt_kmin->setSingleStep( 0.01 );
   ct_plt_kmin->setValue( plt_kmin );
   ct_plt_kmin->setEnabled( false );
   ct_plt_kmax->setRange( 1.0, 50.0 );
   ct_plt_kmax->setSingleStep( 0.01 );
   ct_plt_kmax->setValue( plt_kmax );
   ct_plt_kmax->setEnabled( false );

   plt_smin   = 1.0;
   plt_smax   = 10.0;
   ct_plt_smin->setRange( -10.0, 10000.0 );
   ct_plt_smin->setSingleStep( 0.01 );
   ct_plt_smin->setValue( plt_smin );
   ct_plt_smin->setEnabled( false );
   ct_plt_smax->setRange(   0.0, 10000.0 );
   ct_plt_smax->setSingleStep( 0.01 );
   ct_plt_smax->setValue( plt_smax );
   ct_plt_smax->setEnabled( false );

   // default to white-cyan-magenta-red-black color map
   delete colormap;
   colormap  = new QwtLinearColorMap( Qt::white, Qt::black );
   colormap->addColorStop( 0.10, Qt::cyan );
   colormap->addColorStop( 0.50, Qt::magenta );
   colormap->addColorStop( 0.80, Qt::red );
   cmapname  = tr( "Default Color Map: w-cyan-magenta-red-black" );

   pfilts.clear();
   pb_refresh->setEnabled( true );
}

// plot the data
void US_show_norm::plot_data( void )
{

   zpcent   = ck_zpcent->isChecked();
   DbgLv(1)<<"boolean_variables"<< zpcent<< auto_scz << auto_sxy ;
  
   QList< S_Solute >* sol_d ;

   if ( zpcent )
   {  // Each column norm as a percentage of the sum of all column norms
      data_plot->setAxisTitle( QwtPlot::yRight,
         tr( "Column norm (percent of total)" ) );
      sol_d    = &xy_distro_zp;
   }

   else
   {  // Norm of the A-matrix column, i.e. the L2 norm of the simulated
      //  signal of a single species at unit loading concentration
      data_plot->setAxisTitle( QwtPlot::yRight,
         tr( "Norm of A column (1 OD species)" ) );
      sol_d    = &xy_distro;
   }

   DbgLv(1)<<"sol_d values"<<sol_d->size() ;

   if ( sol_d->size() < 1 )
   {  // Nothing to raster.  US_SpectrogramData::setRaster() returns without
      //  sizing its buffer for an empty list, so drawing a spectrogram here
      //  would index an empty raster.
      data_plot->detachItems( QwtPlotItem::Rtti_PlotSpectrogram );
      data_plot->replot();
      return;
   }

   data_plot->detachItems( QwtPlotItem::Rtti_PlotSpectrogram );

   QwtPlotSpectrogram* d_spectrogram = new QwtPlotSpectrogram();
   US_SpectrogramData* rdata = new US_SpectrogramData();
   d_spectrogram->setData( rdata );
DbgLv(1) << "colormap_before" << colormap;
   d_spectrogram->setColorMap( ColorMapCopy( colormap ) );
   
   US_SpectrogramData& spec_dat = (US_SpectrogramData&)*(d_spectrogram->data());
   QRectF drect;

   if ( auto_sxy )
      drect = QRectF( 0.0, 0.0, 0.0, 0.0 );

   else
   {
      drect = QRectF( plt_smin, plt_kmin,
            ( plt_smax - plt_smin ), ( plt_kmax - plt_kmin ) );
   }

   // Find Z min,max for the current distribution.  This must happen
   //  unconditionally:  when it was done only for auto-scale-Z, plt_zmax
   //  kept its -1e+50 initializer and produced an inverted color-bar
   //  interval and an inverted yRight scale.
   plt_zmin = 1e+50;
   plt_zmax = -1e+50;

   for ( int jj = 0; jj < sol_d->size(); jj++ )
   {
      double zval = sol_d->at( jj ).c;
      plt_zmin    = qMin( plt_zmin, zval );
      plt_zmax    = qMax( plt_zmax, zval );
   }

   if ( plt_zmax <= 0.0 )
   {  // Every norm zero:  use a nominal range so that the color map
      //  and the color bar stay well-defined.
      plt_zmin    = 0.0;
      plt_zmax    = 1.0;
   }

   spec_dat.setRastRanges( xreso, yreso, resolu, zfloor, drect );
   spec_dat.setZRange( 0.0, plt_zmax );
   spec_dat.setRaster( sol_d );

   d_spectrogram->attach( data_plot );

   // set color map and axis settings
   QwtScaleWidget *rightAxis = data_plot->axisWidget( QwtPlot::yRight );
   rightAxis->setColorBarEnabled( true );

   xa_title    = anno_title( plot_x );
   ya_title    = anno_title( plot_y );
   data_plot->setAxisTitle( QwtPlot::xBottom, xa_title );
   data_plot->setAxisTitle( QwtPlot::yLeft,   ya_title );

   if ( auto_sxy )
   { // Auto scale x and y
      data_plot->setAxisAutoScale( QwtPlot::yLeft   );
      data_plot->setAxisAutoScale( QwtPlot::xBottom );
   }
   else
   { // Manual limits on x and y
      double lStep = data_plot->axisStepSize( QwtPlot::yLeft   );
      double bStep = data_plot->axisStepSize( QwtPlot::xBottom );
      data_plot->setAxisScale( QwtPlot::xBottom, plt_smin, plt_smax, bStep );
      data_plot->setAxisScale( QwtPlot::yLeft,   plt_kmin, plt_kmax, lStep );
   }

   rightAxis->setColorMap( QwtInterval( 0.0, plt_zmax ),
                           ColorMapCopy( colormap ) );
   data_plot->setAxisScale( QwtPlot::yRight,  0.0, plt_zmax );

   data_plot->replot();
}

void US_show_norm::plot_data( int )
{
   plot_data();
}

void US_show_norm::update_resolu( double dval )
{
   resolu = dval;
}

void US_show_norm::update_xreso( double dval )
{
   xreso  = dval;
}

void US_show_norm::update_yreso( double dval )
{
   yreso  = dval;
}

void US_show_norm::update_zfloor( double dval )
{
   zfloor = dval;
}

void US_show_norm::update_plot_smin( double dval )
{
   plt_smin = dval;
DbgLv(1) << "plt_smin" << plt_smin;
}

void US_show_norm::update_plot_smax( double dval )
{
   plt_smax = dval;
DbgLv(1) << "plt_smax" << plt_smax;
}

void US_show_norm::update_plot_kmin( double dval )
{
   plt_kmin = dval;
}

void US_show_norm::update_plot_kmax( double dval )
{
   plt_kmax = dval;
}

void US_show_norm::select_autosxy()
{
   auto_sxy   = ck_autosxy->isChecked();
   ct_plt_kmin->setEnabled( !auto_sxy );
   ct_plt_kmax->setEnabled( !auto_sxy );
   ct_plt_smin->setEnabled( !auto_sxy );
   ct_plt_smax->setEnabled( !auto_sxy );

   set_limits();
}

void US_show_norm::select_autoscz()
{
   auto_scz   = ck_autoscz->isChecked();

   set_limits();
}

void US_show_norm::load_color()
{
   QString filter = tr( "Color Map files (*cm-*.xml);;" )
         + tr( "Any XML files (*.xml);;" )
         + tr( "Any files (*)" );

   // get an xml file name for the color map
   QString fname = QFileDialog::getOpenFileName( this,
      tr( "Load Color Map File" ),
      US_Settings::etcDir(), filter );

   if ( fname.isEmpty() )
      return;

   // get the map from the file
   QList< QColor > cmcolor;
   QList< double > cmvalue;

   US_ColorGradIO::read_color_steps( fname, cmcolor, cmvalue );

   if ( cmcolor.size() < 2  ||  cmvalue.size() != cmcolor.size() )
   {  // Unusable color file:  keep the map currently in effect
      QMessageBox::warning( this, tr( "Color Map Error" ),
         tr( "No usable color steps could be read from:\n  %1" ).arg( fname ) );
      return;
   }

   delete colormap;
   colormap  = new QwtLinearColorMap( cmcolor.first(), cmcolor.last() );

   for ( int jj = 1; jj < cmvalue.size() - 1; jj++ )
   {
      colormap->addColorStop( cmvalue.at( jj ), cmcolor.at( jj ) );
   }
   QFileInfo fi( fname );
   cmapname  = tr( "Color Map: " ) + fi.baseName();
   le_cmap_name->setText( cmapname );

   plot_data();
}

void US_show_norm::set_limits()
{
   double smin = 1.0e30;
   double smax = -1.0e30;
   double kmin = 1.0e30;
   double kmax = -1.0e30;
   double sinc;
   double kinc;
   xa_title    = anno_title( plot_x );
   ya_title    = anno_title( plot_y );

   data_plot->setAxisTitle( QwtPlot::xBottom, xa_title );
   data_plot->setAxisTitle( QwtPlot::yLeft,   ya_title );

   if ( xy_distro.size() < 1 )
      return;

   // find min,max for X,Y distributions.  Derive them from the points that
   //  are actually plotted, so that the axis limits cannot disagree with
   //  the raster (the Y attribute is f/f0 or vbar depending on grid type).
   for ( int ii = 0; ii < xy_distro.size(); ii++ )
   {
      double sval = xy_distro[ ii ].s;
      double kval = xy_distro[ ii ].k;
      smin        = qMin( smin, sval );
      smax        = qMax( smax, sval );
      kmin        = qMin( kmin, kval );
      kmax        = qMax( kmax, kval );
   }

   // adjust minima, maxima
   sinc      = ( smax - smin ) / 10.0;
   kinc      = ( kmax - kmin ) / 10.0;
   sinc      = ( sinc <= 0.0 ) ? ( smin * 0.05 ) : sinc;
   kinc      = ( kinc <= 0.0 ) ? ( kmin * 0.05 ) : kinc;
   DbgLv(1) << "SL: real smin smax kmin kmax" << smin << smax << kmin << kmax;
   smin     -= sinc;
   smax     += sinc;
   kmin     -= kinc;
   kmax     += kinc;
   DbgLv(1) << "SL: adjusted smin smax kmin kmax" << smin << smax << kmin << kmax;

   if ( auto_sxy )
   {  // Set auto limits on X and Y
      sinc        = pow( 10.0, qFloor( log10( smax ) ) - 3.0 );
      kinc        = pow( 10.0, qFloor( log10( kmax ) ) - 3.0 );
      if ( qAbs( ( smax - smin ) / smax ) < 0.001 )
      {  // Put padding around virtually constant value
         smin     -= sinc;
         smax     += sinc;
      }
      if ( qAbs( ( kmax - kmin ) / kmax ) < 0.001 )
      {  // Put padding around virtually constant value
         kmin     -= kinc;
         kmax     += kinc;
      }
      // Make sure limits are nearest reasonable values
      smin        = qFloor( smin / sinc ) * sinc;
      smax        = qFloor( smax / sinc ) * sinc + sinc;
      smin        = ( plot_x != ATTR_S ) ? qMax( smin, 0.0 ) : smin;
      smin        = ( plot_x == ATTR_K ) ? qMax( smin, 0.5 ) : smin;
      kmin        = qFloor( kmin / kinc ) * kinc;
      kmax        = qFloor( kmax / kinc ) * kinc + kinc;

      DbgLv(1) << "SL: setVal kmin kmax" << kmin << kmax;
      ct_plt_smin->setValue( smin );
      ct_plt_smax->setValue( smax );
      ct_plt_kmin->setValue( kmin );
      ct_plt_kmax->setValue( kmax );

      plt_smin    = smin;
      plt_smax    = smax;
      plt_kmin    = kmin;
      plt_kmax    = kmax;
   }
   else
   {
      plt_smin    = ct_plt_smin->value();
      plt_smax    = ct_plt_smax->value();
      plt_kmin    = ct_plt_kmin->value();
      plt_kmax    = ct_plt_kmax->value();
   }
   DbgLv(1) << "SL: plt_smin _smax _kmin _kmax" << plt_smin << plt_smax
            << plt_kmin << plt_kmax;
}


// Sort distribution solute list by s,k values and optionally reduce
void US_show_norm::sort_distro( QList< S_Solute >& listsols,
      bool reduce )
{
   int sizi = listsols.size();

   if ( sizi < 2 )
      return;        // nothing need be done for 1-element list

   // sort distro solute list by s,k values

   std::sort( listsols.begin(), listsols.end(), distro_lessthan );

   // check reduce flag

   if ( reduce )
   {     // skip any duplicates in sorted list
      S_Solute sol1;
      S_Solute sol2;
      QList< S_Solute > reduced;
      QList< S_Solute >::iterator jj = listsols.begin();
      sol1     = *jj;
      reduced.append( *jj );     // output first entry
      int kdup = 0;
      int jdup = 0;

      while ( (++jj) != listsols.end() )
      {     // loop to compare each entry to previous
          sol2    = *jj;         // solute entry

          if ( sol1.s != sol2.s  ||  sol1.k != sol2.k )
          {   // not a duplicate, so output to temporary list
             reduced.append( sol2 );
             jdup    = 0;
          }

          else
          {  // duplicate, so sum c value;
             sol2.c += sol1.c;   // sum c value
             sol2.s  = ( sol1.s + sol2.s ) * 0.5;  // average s,k
             sol2.k  = ( sol1.k + sol2.k ) * 0.5;
             reduced.replace( reduced.size() - 1, sol2 );
             kdup    = qMax( kdup, ++jdup );
          }

          sol1    = sol2;        // save entry for next iteration
      }

      if ( kdup > 0 )
      {   // if some reduction happened, replace list with reduced version
         double sc = 1.0 / (double)( kdup + 1 );
DbgLv(1) << "KDUP" << kdup;
//sc = 1.0;

         for ( int ii = 0; ii < reduced.size(); ii++ )
         {  // first scale c values by reciprocal of maximum replicate count
            reduced[ ii ].c *= sc;
         }

         listsols = reduced;
DbgLv(1) << " reduced-size" << reduced.size();
      }
   }
DbgLv(1) << " sol-size" << listsols.size();
   return;
}

// Select coordinate for horizontal axis
void US_show_norm::select_x_axis( int ival )
{
   const QString xlabs[] = {      "s", "f/f0",  "MW", "vbar", "D", "f"  };
   const double  xvlos[] = {      1.0,   1.0,   2e+4,  0.60, 1e-8, 1e-8 };
   const double  xvhis[] = {     10.0,   4.0,   1e+5,  0.80, 1e-7, 1e-7 };
   const double  xmins[] = { -10000.0,   1.0,    0.0,  0.01, 1e-9, 1e-9 };
   const double  xmaxs[] = {  10000.0,  50.0,  1e+10,  3.00, 1e-5, 1e-5 };
   const double  xincs[] = {     0.01,  0.01, 1000.0,  0.01, 1e-9, 1e-9 };

   plot_x     = ival;

   lb_plt_smin->setText( tr( "Plot Limit " ) + xlabs[ plot_x ]
                       + tr( " Minimum:" ) );
   lb_plt_smax->setText( tr( "Plot Limit " ) + xlabs[ plot_x ]
                       + tr( " Maximum:" ) );
   ct_plt_smin->setRange( xmins[ plot_x ], xmaxs[ plot_x ] );
   ct_plt_smax->setRange( xmins[ plot_x ], xmaxs[ plot_x ] );
   ct_plt_smin->setSingleStep( xincs[ plot_x ] );
   ct_plt_smax->setSingleStep( xincs[ plot_x ] );
   ct_plt_smin->setValue( xvlos[ plot_x ] );
   ct_plt_smax->setValue( xvhis[ plot_x ] );

   rb_y_s   ->setEnabled( plot_x != ATTR_S );
   rb_y_ff0 ->setEnabled( plot_x != ATTR_K );
   rb_y_mw  ->setEnabled( plot_x != ATTR_W );
   rb_y_vbar->setEnabled( plot_x != ATTR_V );
   rb_y_D   ->setEnabled( plot_x != ATTR_D );
   rb_y_f   ->setEnabled( plot_x != ATTR_F );

   //build_xy_distro();

   set_limits();

   plot_data();
}

// Select coordinate for vertical axis
void US_show_norm::select_y_axis( int ival )
{
   const QString ylabs[] = {      "s", "f/f0",  "MW", "vbar",    "D", "f"  };
   const double  yvlos[] = {      1.0,   1.0,   2e+4,  0.60,     0.0, 1e-8 };
   const double  yvhis[] = {     10.0,   4.0,   1e+5,  0.80,    30.0, 1e-7 };
   const double  ymins[] = { -10000.0,   1.0,    0.0,  0.01,     0.0, 1e-9 };
   const double  ymaxs[] = {  10000.0,  50.0,  1e+10,  3.00, 10000.0, 1e-5 };
   const double  yincs[] = {     0.01,  0.01, 1000.0,  0.01,    0.01, 1e-9 };

   plot_y     = ival;

   lb_plt_kmin->setText( tr( "Plot Limit " ) + ylabs[ plot_y ]
                       + tr( " Minimum:" ) );
   lb_plt_kmax->setText( tr( "Plot Limit " ) + ylabs[ plot_y ]
                       + tr( " Maximum:" ) );
   ct_plt_kmin->setRange( ymins[ plot_y ], ymaxs[ plot_y ] );
   ct_plt_kmax->setRange( ymins[ plot_y ], ymaxs[ plot_y ] );
   ct_plt_kmin->setSingleStep( yincs[ plot_y ] );
   ct_plt_kmax->setSingleStep( yincs[ plot_y ] );
   ct_plt_kmin->setValue( yvlos[ plot_y ] );
   ct_plt_kmax->setValue( yvhis[ plot_y ] );

   rb_x_s   ->setEnabled( plot_y != ATTR_S );
   rb_x_ff0 ->setEnabled( plot_y != ATTR_K );
   rb_x_mw  ->setEnabled( plot_y != ATTR_W );
   rb_x_vbar->setEnabled( plot_y != ATTR_V );
   rb_x_D   ->setEnabled( plot_y != ATTR_D );
   rb_x_f   ->setEnabled( plot_y != ATTR_F );

   set_limits();

   plot_data();
}

// Re-generate the XY version of the current distribution
void US_show_norm::build_xy_distro()
{
   xy_distro   .clear();
   xy_distro_zp.clear();

   int nsolutes  = model->components.count();

   if ( nsolutes < 1 )
      return;

   xy_distro   .reserve( nsolutes );
   xy_distro_zp.reserve( nsolutes );

   // In the constant-vbar case the grid varies f/f0; in the constant-f/f0
   //  case it varies vbar.  The Y attribute follows whichever one varies.
   plot_y        = cnst_vbar ? ATTR_K : ATTR_V;

   S_Solute sol;
   double tot_conc  = 0.0;

   for ( int ii = 0; ii < nsolutes; ii++ )
   {
      sol.s      = model->components[ ii ].s;
      sol.k      = ( plot_y == ATTR_K ) ? model->components[ ii ].f_f0
                                        : model->components[ ii ].vbar20;
      sol.c      = model->components[ ii ].signal_concentration;
      xy_distro << sol;
      tot_conc  += sol.c;
   }

   // Percentage-of-total version.  Guard against an all-zero norm set
   //  (e.g. every column below the NNLS norm cutoff).
   double zscale = ( tot_conc > 0.0 ) ? ( 100.0 / tot_conc ) : 0.0;

   for ( int ii = 0; ii < nsolutes; ii++ )
   {
      sol        = xy_distro[ ii ];
      sol.c     *= zscale;
      xy_distro_zp << sol;
   }
}

// Set annotation title for a plot index
QString US_show_norm::anno_title( int pltndx )
{
   QString a_title;

   if      ( pltndx == ATTR_S )
      a_title  = tr( "Sedimentation Coefficient (1e-13)"
                     " for water at 20" ) + DEGC;
   else if ( pltndx == ATTR_K )
      a_title  = tr( "Frictional Ratio f/f0" );
   else if ( pltndx == ATTR_W )
      a_title  = tr( "Molecular Weight (Dalton)" );
   else if ( pltndx == ATTR_V )
      a_title  = tr( "Vbar at 20" ) + DEGC;
   else if ( pltndx == ATTR_D )
      a_title  = tr( "Diffusion Coefficient (1e-7)" );
   else if ( pltndx == ATTR_F )
      a_title  = tr( "Frictional Coefficient" );

   return a_title;
}

// Make a ColorMap copy and return a pointer to the new ColorMap
QwtLinearColorMap* US_show_norm::ColorMapCopy( QwtLinearColorMap* colormap )
{
   DbgLv(1)<< "colorMapCopy_is called"  ;
   DbgLv(1) << "colormap" << colormap;

   QVector< double >  cstops   = colormap->colorStops();
   DbgLv(1)<< "after colormap->colorStops()"  ;

   int                lstop    = cstops.count() - 1;
   DbgLv(1)<< "lstop_ColorMapCopy" << lstop ;
   QwtInterval        csvals( 0.0, 1.0 );
   QwtLinearColorMap* cmapcopy = new QwtLinearColorMap( colormap->color1(),
                                                        colormap->color2() );

   for ( int jj = 1; jj < lstop; jj++ )
   {
      QColor scolor = colormap->color( csvals, cstops[ jj ] );
      cmapcopy->addColorStop( cstops[ jj ], scolor );
   }

   return cmapcopy;
}

