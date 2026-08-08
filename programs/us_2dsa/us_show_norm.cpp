//! \file us_show_norm.cpp

#include "us_show_norm.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_gui_util.h"
#include "us_math2.h"
#include "us_constants.h"
#include "qwt_picker_machine.h"
#include <algorithm>

//------------------------------------------------------------------------
// A window over the A-matrix column norms of a 2DSA grid.
//
// Each grid point (X,Y) carries the L2 norm of the simulated signal that a
// single species of unit loading concentration would produce.  That is
// exactly column j of the A matrix the NNLS solver is handed, so the map
// says which parts of the grid can contribute signal at all, and the
// companion collinearity maps say which parts of the grid NNLS cannot tell
// apart.  The norm alone answers the first question; only the coherence
// between neighbouring columns answers the second, which is why both are
// offered here.
//------------------------------------------------------------------------

US_NormRasterData::US_NormRasterData() : QwtRasterData()
{
   xmin     = 0.0;
   ymin     = 0.0;
   xmax     = 1.0;
   ymax     = 1.0;
   uscl     = 1.0;
   vscl     = 1.0;
   zmin     = 0.0;
   zmax     = 1.0;
   cellm    = 1.0;
   nbx      = 1;
   nby      = 1;
   npts     = 0;
}

// Define the points, their Z range, and the plotted rectangle
void US_NormRasterData::setPoints( const QVector< double >& xpos,
      const QVector< double >& ypos, const QVector< double >& zval,
      double a_zmin, double a_zmax, const QRectF& drect )
{
   npts     = qMin( xpos.size(), qMin( ypos.size(), zval.size() ) );
   xmin     = drect.left  ();
   xmax     = drect.right ();
   ymin     = drect.top   ();
   ymax     = drect.bottom();
   zmin     = a_zmin;
   zmax     = a_zmax;
   uscl     = 1.0 / qMax( xmax - xmin, 1.0e-300 );
   vscl     = 1.0 / qMax( ymax - ymin, 1.0e-300 );

#if QWT_VERSION < QT_VERSION_CHECK(6, 3, 0)
   setInterval( Qt::XAxis, QwtInterval( xmin, xmax ) );
   setInterval( Qt::YAxis, QwtInterval( ymin, ymax ) );
   setInterval( Qt::ZAxis, QwtInterval( zmin, zmax ) );
#endif

   upos.resize( npts );
   vpos.resize( npts );
   zpos.resize( npts );

   // About one point per bucket, so that a lookup normally resolves within
   //  the query bucket and its immediate neighbours
   int nbuc = qBound( 4, (int)qRound( sqrt( (double)qMax( npts, 1 ) ) ), 256 );
   nbx      = nbuc;
   nby      = nbuc;
   cellm    = qMin( 1.0 / (double)nbx, 1.0 / (double)nby );

   bkts.clear();
   bkts.resize( nbx * nby );

   for ( int ii = 0; ii < npts; ii++ )
   {
      double uu    = ( xpos[ ii ] - xmin ) * uscl;
      double vv    = ( ypos[ ii ] - ymin ) * vscl;
      upos[ ii ]   = uu;
      vpos[ ii ]   = vv;
      zpos[ ii ]   = zval[ ii ];

      int    bx    = qBound( 0, (int)( uu * (double)nbx ), nbx - 1 );
      int    by    = qBound( 0, (int)( vv * (double)nby ), nby - 1 );
      bkts[ by * nbx + bx ] << ii;
   }
}

QwtInterval US_NormRasterData::interval( Qt::Axis axis ) const
{
   switch ( axis )
   {
      case Qt::XAxis:  return QwtInterval( xmin, xmax );
      case Qt::YAxis:  return QwtInterval( ymin, ymax );
      case Qt::ZAxis:
      default:         return QwtInterval( zmin, zmax );
   }
}

// Z value of the grid point nearest a plot position
double US_NormRasterData::value( double x, double y ) const
{
   if ( npts < 1 )
      return zmin;

   double uu      = ( x - xmin ) * uscl;
   double vv      = ( y - ymin ) * vscl;
   int    bx      = qBound( 0, (int)( uu * (double)nbx ), nbx - 1 );
   int    by      = qBound( 0, (int)( vv * (double)nby ), nby - 1 );
   int    maxr    = qMax( nbx, nby );
   int    besti   = -1;
   double bestd   = 1.0e+300;

   for ( int rr = 0; rr <= maxr; rr++ )
   {  // Examine the ring of buckets at radius rr around the query bucket
      int jy0     = by - rr;
      int jy1     = by + rr;
      int jx0     = bx - rr;
      int jx1     = bx + rr;

      for ( int jy = jy0; jy <= jy1; jy++ )
      {
         if ( jy < 0  ||  jy >= nby )  continue;

         bool edgey  = ( jy == jy0  ||  jy == jy1 );

         for ( int jx = jx0; jx <= jx1; jx++ )
         {
            if ( jx < 0  ||  jx >= nbx )  continue;
            // Interior buckets belong to a smaller ring already examined
            if ( ! edgey  &&  jx != jx0  &&  jx != jx1 )  continue;

            const QVector< int >& bkt = bkts[ jy * nbx + jx ];

            for ( int kk = 0; kk < bkt.size(); kk++ )
            {
               int    ii      = bkt[ kk ];
               double du      = uu - upos[ ii ];
               double dv      = vv - vpos[ ii ];
               double dd      = ( du * du ) + ( dv * dv );

               if ( dd < bestd )
               {
                  bestd          = dd;
                  besti          = ii;
               }
            }
         }
      }

      // Nothing in a wider ring can be closer than rr bucket widths away
      double reach   = (double)rr * cellm;

      if ( besti >= 0  &&  bestd <= ( reach * reach ) )
         break;
   }

   return ( besti >= 0 ) ? zpos[ besti ] : zmin;
}

//------------------------------------------------------------------------

US_show_norm::US_show_norm( US_Model* a_model, bool a_cnst_vbar,
      const US_NormGridInfo& a_ginfo, QWidget* p )
   : US_WidgetsDialog( p, Qt::WindowFlags() )
{
   cnst_vbar      = a_cnst_vbar;
   model          = a_model;      // must be set before any set_limits() call
   ginfo          = a_ginfo;
   parentw        = p;
   dbg_level      = US_Settings::us_debug();
   colormap       = NULL;
   zmode          = ZM_NORM;
   pick_ix        = 0;
   pick_iy        = 0;

   int nsolutes   = model->components.count();

   // A usable grid needs a shape that accounts for every point
   have_grid      = ( ginfo.nss > 0  &&  ginfo.nks > 0  &&
                      ( ginfo.nss * ginfo.nks ) == nsolutes );
   have_sigs      = ( have_grid  &&  ginfo.nsamp > 0  &&
                      ginfo.sigs.size() == ( nsolutes * ginfo.nsamp ) );
   // The arrays are sized even when nothing filled them, so require that at
   //  least one real coherence came back before offering those modes
   have_coher     = ( have_grid  &&
                      ginfo.coher_x.size() == nsolutes  &&
                      ginfo.coher_y.size() == nsolutes );

   if ( have_coher )
   {
      bool anycoh    = false;

      for ( int ii = 0; ii < nsolutes  &&  ! anycoh; ii++ )
         anycoh         = ( ginfo.coher_x[ ii ] >= 0.0  ||
                            ginfo.coher_y[ ii ] >= 0.0 );

      have_coher     = anycoh;
   }
   have_cut       = ( ginfo.norm_cut > 0.0 );

   // A column norm is a sum in quadrature over every data point, so its
   //  magnitude depends on how many scans and radial points the experiment
   //  has.  Dividing by the square root of that count gives the RMS signal
   //  per data point, in OD, which is directly interpretable and comparable
   //  between runs.
   have_rms       = ( ginfo.ndpts > 0 );
   rms_scale      = have_rms ? ( 1.0 / sqrt( (double)ginfo.ndpts ) ) : 1.0;

   // Column norms, kept apart from the model so that the Z modes can be
   //  recomputed without touching it
   gnorm.resize( nsolutes );

   for ( int ii = 0; ii < nsolutes; ii++ )
      gnorm[ ii ]    = model->components[ ii ].signal_concentration;

   setWindowTitle( tr( "2DSA Norm Grid and Conditioning" ) );
   setPalette( US_GuiSettings::frameColor() );

   // primary layouts
   QHBoxLayout* main  = new QHBoxLayout( this );
   QVBoxLayout* left  = new QVBoxLayout();
   QGridLayout* spec  = new QGridLayout();
   QVBoxLayout* right = new QVBoxLayout();
   QHBoxLayout* xsecs = new QHBoxLayout();
   main->setSpacing        ( 2 );
   main->setContentsMargins( 2, 2, 2, 2 );
   left->setSpacing        ( 0 );
   left->setContentsMargins( 0, 1, 0, 1 );
   spec->setSpacing        ( 1 );
   spec->setContentsMargins( 0, 0, 0, 0 );

   int s_row = 0;

   QLabel* lb_info1      = us_banner( tr( "Norms and conditioning of the"
                                          " A-matrix columns" ) );

   // Summary statistics.  This is the part that answers "does f/f0 matter
   //  here" with a number rather than with a color.
   te_distr_info = us_textedit();
   us_setReadOnly( te_distr_info, true );
   te_distr_info->setLineWrapMode( QTextEdit::NoWrap );
   te_distr_info->setMinimumHeight( 190 );
   te_distr_info->setFont( QFont( "monospace",
                                  US_GuiSettings::fontSize() - 1 ) );

   // Z quantity selection
   QLabel* lb_zmode      = us_label( tr( "Color (Z) shows:" ) );
   lb_zmode->setAlignment( Qt::AlignVCenter | Qt::AlignLeft );

   cb_zmode      = us_comboBox();
   cb_zmode->addItem( tr( "Column norm  ||a||"                    ) );
   cb_zmode->addItem( tr( "RMS signal per point  ||a||/sqrt(N)"   ) );
   cb_zmode->addItem( tr( "Column norm  log10(||a||)"             ) );
   cb_zmode->addItem( tr( "Norm relative to max at same X  (%)"   ) );
   cb_zmode->addItem( tr( "Norm deviation from mean over Y  (%)"  ) );
   cb_zmode->addItem( tr( "Norm as percent of all norms  (%)"     ) );
   cb_zmode->addItem( tr( "Collinearity with SELECTED point"      ) );
   cb_zmode->addItem( tr( "Collinearity with X neighbour"         ) );
   cb_zmode->addItem( tr( "Collinearity with Y neighbour"         ) );
   cb_zmode->addItem( tr( "Collinearity, worst neighbour"         ) );
   cb_zmode->setCurrentIndex( ZM_NORM );
   cb_zmode->setToolTip( tr(
      "The norm varies over orders of magnitude along s but only by a few\n"
      "percent along f/f0, so a single linear scale hides the f/f0\n"
      "dependence entirely.  The relative and deviation modes rescale each\n"
      "X column so that dependence becomes visible.\n\n"
      "The collinearity modes show how nearly parallel two A columns are,\n"
      "which is what limits how well NNLS can separate them.  In every\n"
      "collinearity mode a HIGHER value means HARDER to separate.\n\n"
      "\"With SELECTED point\" compares every grid point against the one\n"
      "clicked in the map:  it shows the whole region of parameter space\n"
      "that this experiment cannot distinguish from that point.  The\n"
      "neighbour modes instead compare each point with the one next to it,\n"
      "which says whether the grid itself is finer than the data warrant." ) );

   if ( ! have_grid )
   {  // Column-relative modes need a known grid shape
      cb_zmode->setItemData( ZM_RELX, 0, Qt::UserRole - 1 );
      cb_zmode->setItemData( ZM_DEVX, 0, Qt::UserRole - 1 );
   }

   if ( ! have_coher )
   {  // Neighbour collinearity modes need the coherence values
      cb_zmode->setItemData( ZM_COH_X,   0, Qt::UserRole - 1 );
      cb_zmode->setItemData( ZM_COH_Y,   0, Qt::UserRole - 1 );
      cb_zmode->setItemData( ZM_COH_MAX, 0, Qt::UserRole - 1 );
   }

   if ( ! have_sigs )
   {  // Comparing against an arbitrary point needs the column signatures
      cb_zmode->setItemData( ZM_COH_PICK, 0, Qt::UserRole - 1 );
   }

   if ( ! have_rms )
   {  // The RMS mode needs the data point count
      cb_zmode->setItemData( ZM_RMS, 0, Qt::UserRole - 1 );
   }

   connect( cb_zmode, SIGNAL( currentIndexChanged( int ) ),
            this,     SLOT  ( select_zmode       ( int ) ) );

   // NNLS cutoff marking
   us_checkbox( tr( "Mark columns dropped by NNLS" ), ck_cutmark, true );
   ck_cutmark->setEnabled( have_cut );
   ck_cutmark->setToolTip( tr(
      "A column whose norm falls below the NNLS cutoff (%1) is removed from\n"
      "the fit entirely:  that part of the grid cannot contribute to the\n"
      "solution no matter what the data contain." )
      .arg( ginfo.norm_cut, 0, 'g', 4 ) );
   connect( ck_cutmark, SIGNAL( clicked() ),
            this,       SLOT  ( select_cutmark() ) );

   // Read-out for the point last clicked in the map
   le_pickinfo   = us_lineedit( tr( "Click the map to select a grid point" ),
                                -1, true );

   // Axis attribute selection
           plot_x      = ATTR_S;
           plot_y      = ATTR_K;
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
   connect( bg_x_axis, &QButtonGroup::idReleased,
            this, &US_show_norm::select_x_axis );
   connect( bg_y_axis, &QButtonGroup::idReleased,
            this, &US_show_norm::select_y_axis );

   // Raster and scaling controls
   us_checkbox( tr( "Autoscale X and Y" ), ck_autosxy, true );
   connect( ck_autosxy, SIGNAL( clicked() ),
            this,       SLOT( select_autosxy() ) );

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

   le_cmap_name  = us_lineedit(
         tr( "Default Color Map: w-cyan-magenta-red-black" ), -1, true );

   pb_refresh    = us_pushbutton( tr( "Refresh" ) );
   connect( pb_refresh, SIGNAL( clicked() ),
            this,       SLOT( plot_data() ) );

   pb_reset      = us_pushbutton( tr( "Reset" ) );
   connect( pb_reset,   SIGNAL( clicked() ),
            this,       SLOT( reset() ) );

   pb_ldcolor    = us_pushbutton( tr( "Load Color File" ) );
   connect( pb_ldcolor, SIGNAL( clicked() ),
            this,       SLOT( load_color() ) );

   pb_close      = us_pushbutton( tr( "Close" ) );
   connect( pb_close,   SIGNAL( clicked() ),
            this,       SLOT( close() ) );

   QFontMetrics fm( ct_plt_smax->font() );
   ct_plt_smax->adjustSize();
   ct_plt_smax->setMinimumWidth( ct_plt_smax->width()
                                 + fm.horizontalAdvance( "ABC" ) );

   // Order plot components on the left side
   spec->addWidget( lb_info1,      s_row++, 0, 1, 8 );
   spec->addWidget( te_distr_info, s_row,   0, 6, 8 ); s_row += 6;
   spec->addWidget( lb_zmode,      s_row,   0, 1, 3 );
   spec->addWidget( cb_zmode,      s_row++, 3, 1, 5 );
   spec->addWidget( ck_cutmark,    s_row++, 0, 1, 8 );
   spec->addWidget( le_pickinfo,   s_row++, 0, 1, 8 );
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
   spec->addWidget( ck_autosxy,    s_row++, 0, 1, 8 );
   spec->addWidget( lb_plt_smin,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_smin,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_smax,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_smax,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_kmin,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_kmin,   s_row++, 4, 1, 4 );
   spec->addWidget( lb_plt_kmax,   s_row,   0, 1, 4 );
   spec->addWidget( ct_plt_kmax,   s_row++, 4, 1, 4 );
   spec->addWidget( le_cmap_name,  s_row++, 0, 1, 8 );
   spec->addWidget( pb_refresh,    s_row++, 0, 1, 8 );
   spec->addWidget( pb_ldcolor,    s_row++, 0, 1, 8 );
   spec->addWidget( pb_reset,      s_row++, 0, 1, 8 );
   spec->addWidget( pb_close,      s_row++, 0, 1, 8 );

   // The map on the right, with the two cross sections beneath it
   xa_title    = anno_title( ATTR_S );
   ya_title    = anno_title( ATTR_K );
   QBoxLayout* plot = new US_Plot( data_plot,
      tr( "Norm grid" ), xa_title, ya_title );

   data_plot->setMinimumSize( 600, 420 );
   data_plot->enableAxis( QwtPlot::xBottom, true );
   data_plot->enableAxis( QwtPlot::yLeft,   true );
   data_plot->enableAxis( QwtPlot::yRight,  true );
   data_plot->setCanvasBackground( Qt::white );

   QBoxLayout* xsy = new US_Plot( xsec_plot_y,
      tr( "Section along Y" ), ya_title, tr( "Norm" ) );
   QBoxLayout* xsx = new US_Plot( xsec_plot_x,
      tr( "Section along X" ), xa_title, tr( "Norm" ) );

   xsec_plot_y->setMinimumSize( 280, 220 );
   xsec_plot_x->setMinimumSize( 280, 220 );
   xsec_plot_y->setCanvasBackground( Qt::white );
   xsec_plot_x->setCanvasBackground( Qt::white );

   // Clicking the map selects the grid point the sections are taken through
   pick = new US_PlotPicker( data_plot );
   pick->setStateMachine( new QwtPickerClickPointMachine() );
   pick->setRubberBand( QwtPicker::CrossRubberBand );
   connect( pick, SIGNAL( mouseDown ( const QPointF& ) ),
            this, SLOT  ( pick_point( const QPointF& ) ) );

   xsecs->addLayout( xsy );
   xsecs->addLayout( xsx );

   right->addLayout( plot  );
   right->addLayout( xsecs );
   right->setStretchFactor( plot,  3 );
   right->setStretchFactor( xsecs, 2 );

   left->addLayout( spec );
   left->addStretch();

   main->addLayout( left  );
   main->addLayout( right );
   main->setStretchFactor( left,  3 );
   main->setStretchFactor( right, 6 );

   // Start the cross sections through the middle of the grid
   pick_ix     = have_grid ? ( ginfo.nss / 2 ) : 0;
   pick_iy     = have_grid ? ( ginfo.nks / 2 ) : 0;

   // Establish the default attributes, scales and color map, sync the axis
   //  selectors to them, and draw
   reset();
}

US_show_norm::~US_show_norm()
{
   delete colormap;
   colormap   = NULL;
}

void US_show_norm::reset( void )
{
   plot_x     = ATTR_S;
   // Y follows the attribute that actually varies over the grid, so that
   //  Reset cannot leave the axis labelled f/f0 while vbar is plotted.
   plot_y     = cnst_vbar ? ATTR_K : ATTR_V;

   auto_sxy   = true;
   ck_autosxy->setChecked( auto_sxy );

   ct_plt_kmin->setEnabled( false );
   ct_plt_kmax->setEnabled( false );
   ct_plt_smin->setEnabled( false );
   ct_plt_smax->setEnabled( false );

   // default to white-cyan-magenta-red-black color map
   delete colormap;
   colormap  = new QwtLinearColorMap( Qt::white, Qt::black );
   colormap->addColorStop( 0.10, Qt::cyan );
   colormap->addColorStop( 0.50, Qt::magenta );
   colormap->addColorStop( 0.80, Qt::red );
   cmapname  = tr( "Default Color Map: w-cyan-magenta-red-black" );
   le_cmap_name->setText( cmapname );

   zmode      = ZM_NORM;
   cb_zmode->blockSignals( true );      // one redraw at the end, not two
   cb_zmode->setCurrentIndex( zmode );
   cb_zmode->blockSignals( false );

   ck_cutmark->setChecked( have_cut );

   sync_axis_buttons();
   apply_axis_ranges();
   set_limits();
   plot_data();
}

// Value of one model attribute for one grid point
double US_show_norm::attr_value( int ii, int attr )
{
   const US_Model::SimulationComponent& mc = model->components[ ii ];

   switch ( attr )
   {
      case ATTR_K:  return mc.f_f0;
      case ATTR_W:  return mc.mw;
      case ATTR_V:  return mc.vbar20;
      case ATTR_D:  return mc.D * 1.0e7;
      case ATTR_F:  return mc.f;
      case ATTR_S:
      default:      return mc.s;      // already scaled to 1e-13 units
   }
}

// Short name of a plot attribute, for use inside running text
QString US_show_norm::attr_label( int attr )
{
   switch ( attr )
   {
      case ATTR_K:  return tr( "f/f0" );
      case ATTR_W:  return tr( "MW"   );
      case ATTR_V:  return tr( "vbar" );
      case ATTR_D:  return tr( "D"    );
      case ATTR_F:  return tr( "f"    );
      case ATTR_S:
      default:      return tr( "s"    );
   }
}

// Cosine of the angle between two A columns.
//
// The retained signatures are unit length, so their dot product is the
// cosine directly.  They are subsampled, which puts a floor of roughly
// 1e-5 on how close to 1 the result can be resolved; the value is a lower
// bound on the true collinearity, never an overstatement of separability.
double US_show_norm::coher_pair( int ia, int ib )
{
   if ( ! have_sigs  ||  ia < 0  ||  ib < 0 )
      return -1.0;

   int          nsamp = ginfo.nsamp;
   const float* siga  = ginfo.sigs.constData() + ( (size_t)ia * nsamp );
   const float* sigb  = ginfo.sigs.constData() + ( (size_t)ib * nsamp );
   double       dotp  = 0.0;

   for ( int jj = 0; jj < nsamp; jj++ )
      dotp              += ( (double)siga[ jj ] * (double)sigb[ jj ] );

   return qBound( 0.0, dotp, 1.0 );
}

// True when the current Z mode maps a collinearity rather than a norm
bool US_show_norm::zmode_is_coher( void )
{
   return ( zmode == ZM_COH_PICK  ||  zmode == ZM_COH_X  ||
            zmode == ZM_COH_Y     ||  zmode == ZM_COH_MAX );
}

// Title of the Z axis, per the current Z mode
QString US_show_norm::zmode_title( void )
{
   switch ( zmode )
   {
      case ZM_RMS:      return tr( "RMS signal per point (OD)"
                                   " for a 1 OD species" );
      case ZM_LOGNORM:  return tr( "log10 of column norm ||a||" );
      case ZM_RELX:     return tr( "Norm, percent of max at same X" );
      case ZM_DEVX:     return tr( "Norm deviation from Y-mean (%)" );
      case ZM_PCTTOT:   return tr( "Norm, percent of all norms" );
      case ZM_COH_PICK: return tr( "Collinearity with selected point"
                                   "  -log10(1-cos), higher = inseparable" );
      case ZM_COH_X:    return tr( "Collinearity, X neighbour"
                                   "  -log10(1-cos), higher = inseparable" );
      case ZM_COH_Y:    return tr( "Collinearity, Y neighbour"
                                   "  -log10(1-cos), higher = inseparable" );
      case ZM_COH_MAX:  return tr( "Collinearity, worst neighbour"
                                   "  -log10(1-cos), higher = inseparable" );
      case ZM_NORM:
      default:          return tr( "Column norm ||a|| (OD, quadrature sum"
                                   " over %1 points)" ).arg( ginfo.ndpts );
   }
}

// Fill zvals from the norms and the coherences, per the current Z mode.
//
// Every mode leaves zvals in units the color bar and the cross sections can
// both label directly.  The raster itself is fed a shifted copy, because
// US_SpectrogramData initializes untouched raster cells to zero and a mode
// with a negative minimum would otherwise render those cells mid-scale.
void US_show_norm::compute_zvals( void )
{
   int nsol       = gnorm.size();
   zvals .resize( nsol );
   zvalid.fill( true, nsol );

   if ( nsol < 1 )
      return;

   if ( zmode == ZM_COH_PICK  &&  ! have_sigs )
      zmode          = ZM_NORM;      // Signatures were never retained

   if ( zmode_is_coher()  &&  zmode != ZM_COH_PICK  &&  ! have_coher )
      zmode          = ZM_NORM;      // Coherence was never computed

   int nss        = have_grid ? ginfo.nss : nsol;
   int nks        = have_grid ? ginfo.nks : 1;

   switch ( zmode )
   {
      case ZM_RMS:

         for ( int ii = 0; ii < nsol; ii++ )
            zvals[ ii ]    = gnorm[ ii ] * rms_scale;

         break;

      case ZM_LOGNORM:
      {  // Norms reach zero at the pelleting edge, so floor the log well
         //  below the smallest norm that still carries signal
         double zmaxv   = 0.0;

         for ( int ii = 0; ii < nsol; ii++ )
            zmaxv          = qMax( zmaxv, gnorm[ ii ] );

         double zfl     = qMax( zmaxv * 1.0e-8, 1.0e-30 );

         for ( int ii = 0; ii < nsol; ii++ )
            zvals[ ii ]    = log10( qMax( gnorm[ ii ], zfl ) );
      }
      break;

      case ZM_RELX:
      case ZM_DEVX:
      {  // Rescale within each X column.  This is what makes a few-percent
         //  Y dependence visible next to a norm range that spans the whole
         //  grid along X.
         for ( int ix = 0; ix < nss; ix++ )
         {
            double cmax    = 0.0;
            double csum    = 0.0;

            for ( int iy = 0; iy < nks; iy++ )
            {
               double zval    = gnorm[ iy * nss + ix ];
               cmax           = qMax( cmax, zval );
               csum          += zval;
            }

            double cmean   = csum / (double)nks;

            for ( int iy = 0; iy < nks; iy++ )
            {
               int    jj      = iy * nss + ix;

               if ( zmode == ZM_RELX )
                  zvals[ jj ]    = ( cmax > 0.0 )
                                 ? ( gnorm[ jj ] * 100.0 / cmax ) : 0.0;
               else
                  zvals[ jj ]    = ( cmean > 0.0 )
                                 ? ( ( gnorm[ jj ] - cmean ) * 100.0 / cmean )
                                 : 0.0;
            }
         }
      }
      break;

      case ZM_PCTTOT:
      {
         double csum    = 0.0;

         for ( int ii = 0; ii < nsol; ii++ )
            csum          += gnorm[ ii ];

         double cscl    = ( csum > 0.0 ) ? ( 100.0 / csum ) : 0.0;

         for ( int ii = 0; ii < nsol; ii++ )
            zvals[ ii ]    = gnorm[ ii ] * cscl;
      }
      break;

      case ZM_COH_PICK:
      {  // Cosine of every column against the selected one.  This is the
         //  resolution profile of the fit around that point:  it shows how
         //  far one has to move through parameter space before the data can
         //  tell the difference, which the neighbour maps cannot say.
         int    kpick   = qBound( 0, pick_iy, nks - 1 ) * nss
                        + qBound( 0, pick_ix, nss - 1 );
         double zhi     = -1.0e+30;

         for ( int ii = 0; ii < nsol; ii++ )
         {
            if ( ii == kpick )
            {  // Its cosine with itself is 1 by construction and would run
               //  off the top of the scale; give it the worst real value
               //  found, below.
               zvalid[ ii ]   = false;
               continue;
            }

            double dcoh    = qMax( 1.0 - coher_pair( kpick, ii ), 1.0e-9 );
            zvals[ ii ]    = -log10( dcoh );
            zhi            = qMax( zhi, zvals[ ii ] );
         }

         zvals[ kpick ] = ( zhi > -1.0e+29 ) ? zhi : 0.0;
      }
      break;

      case ZM_COH_X:
      case ZM_COH_Y:
      case ZM_COH_MAX:
      {  // Map the cosine through -log10(1-cos) so that the interesting
         //  range, cos between 0.99 and 0.999999, occupies most of the
         //  scale instead of being crushed against 1.
         for ( int ii = 0; ii < nsol; ii++ )
         {
            double cohx    = ginfo.coher_x[ ii ];
            double cohy    = ginfo.coher_y[ ii ];
            double cohv    = ( zmode == ZM_COH_X ) ? cohx
                           : ( zmode == ZM_COH_Y ) ? cohy
                                                   : qMax( cohx, cohy );

            if ( cohv < 0.0 )
            {  // Edge point with no neighbour on that side
               zvalid[ ii ]   = false;
               continue;
            }

            double dcoh    = qMax( 1.0 - cohv, 1.0e-9 );
            zvals[ ii ]    = -log10( dcoh );
         }

         // The map has to paint the edge points, so give each the value of
         //  the neighbour it was compared against.  That repeats a cell
         //  rather than inventing a cliff at the edge of the grid; the
         //  cross sections skip them entirely.
         for ( int ii = 0; ii < nsol; ii++ )
         {
            if ( zvalid[ ii ] )
               continue;

            int jj         = ( zmode == ZM_COH_Y ) ? ( ii - nss ) : ( ii - 1 );
            jj             = ( jj >= 0  &&  jj < nsol  &&  zvalid[ jj ] )
                           ? jj : -1;
            zvals[ ii ]    = ( jj >= 0 ) ? zvals[ jj ] : 0.0;
         }
      }
      break;

      case ZM_NORM:
      default:

         for ( int ii = 0; ii < nsol; ii++ )
            zvals[ ii ]    = gnorm[ ii ];

         break;
   }
}

// Rebuild the plotted point list from the current attribute and Z selections
void US_show_norm::build_xy_distro( void )
{
   xy_distro.clear();

   int nsol       = model->components.count();

   if ( nsol < 1 )
      return;

   compute_zvals();

   plt_zmin       = 1.0e+30;
   plt_zmax       = -1.0e+30;

   for ( int ii = 0; ii < nsol; ii++ )
   {
      plt_zmin       = qMin( plt_zmin, zvals[ ii ] );
      plt_zmax       = qMax( plt_zmax, zvals[ ii ] );
   }

   if ( plt_zmax <= plt_zmin )
   {  // Constant Z:  give the color map a range it can divide by
      plt_zmax       = plt_zmin + 1.0;
   }

   xy_distro.reserve( nsol );
   S_Solute sol;

   for ( int ii = 0; ii < nsol; ii++ )
   {
      sol.s          = attr_value( ii, plot_x );
      sol.k          = attr_value( ii, plot_y );
      sol.c          = zvals[ ii ];
      xy_distro << sol;
   }
}

// plot the data
void US_show_norm::plot_data( void )
{
   build_xy_distro();

   data_plot->detachItems( QwtPlotItem::Rtti_PlotSpectrogram );
   data_plot->detachItems( QwtPlotItem::Rtti_PlotCurve       );
   data_plot->detachItems( QwtPlotItem::Rtti_PlotMarker      );

   if ( xy_distro.size() < 1 )
   {  // Nothing to raster.  US_SpectrogramData::setRaster() returns without
      //  sizing its buffer for an empty list, so drawing a spectrogram here
      //  would index an empty raster.
      data_plot->replot();
      update_stats();
      plot_sections();
      return;
   }

   xa_title    = anno_title( plot_x );
   ya_title    = anno_title( plot_y );
   data_plot->setAxisTitle( QwtPlot::xBottom, xa_title );
   data_plot->setAxisTitle( QwtPlot::yLeft,   ya_title );
   data_plot->setAxisTitle( QwtPlot::yRight,  zmode_title() );
   data_plot->setTitle( zmode_title() );

   // Work out the plotted rectangle.  An explicit rectangle is always
   //  supplied:  with a null one, US_SpectrogramData derives its own Z
   //  range and pins the bottom of it to zero, which is what made every
   //  norm render as the same end-of-scale color.
   double xmin, xmax, ymin, ymax;

   if ( auto_sxy )
   {
      xmin        = 1.0e+30;
      xmax        = -1.0e+30;
      ymin        = 1.0e+30;
      ymax        = -1.0e+30;

      for ( int ii = 0; ii < xy_distro.size(); ii++ )
      {
         xmin        = qMin( xmin, xy_distro[ ii ].s );
         xmax        = qMax( xmax, xy_distro[ ii ].s );
         ymin        = qMin( ymin, xy_distro[ ii ].k );
         ymax        = qMax( ymax, xy_distro[ ii ].k );
      }

      // Half a grid cell of padding, so that the edge cells are fully drawn
      double xpad = ( xmax > xmin ) ? ( ( xmax - xmin ) * 0.02 )
                                    : qMax( qAbs( xmax ) * 0.05, 1.0e-6 );
      double ypad = ( ymax > ymin ) ? ( ( ymax - ymin ) * 0.02 )
                                    : qMax( qAbs( ymax ) * 0.05, 1.0e-6 );
      xmin       -= xpad;
      xmax       += xpad;
      ymin       -= ypad;
      ymax       += ypad;

      ct_plt_smin->setValue( xmin );
      ct_plt_smax->setValue( xmax );
      ct_plt_kmin->setValue( ymin );
      ct_plt_kmax->setValue( ymax );
   }

   else
   {
      xmin        = plt_smin;
      xmax        = plt_smax;
      ymin        = plt_kmin;
      ymax        = plt_kmax;

      if ( xmax <= xmin )  xmax = xmin + 1.0e-6;
      if ( ymax <= ymin )  ymax = ymin + 1.0e-6;
   }

   plt_smin    = xmin;
   plt_smax    = xmax;
   plt_kmin    = ymin;
   plt_kmax    = ymax;

   // One flat cell per grid point, with hard edges:  no value is invented
   //  between grid points and none is blurred away.
   QVector< double > rx;
   QVector< double > ry;
   QVector< double > rz;
   rx.reserve( xy_distro.size() );
   ry.reserve( xy_distro.size() );
   rz.reserve( xy_distro.size() );

   for ( int ii = 0; ii < xy_distro.size(); ii++ )
   {
      rx << xy_distro[ ii ].s;
      ry << xy_distro[ ii ].k;
      rz << zvals    [ ii ];
   }

   QRectF drect( xmin, ymin, ( xmax - xmin ), ( ymax - ymin ) );

   QwtPlotSpectrogram* d_spectrogram = new QwtPlotSpectrogram();
   US_NormRasterData*  rdata         = new US_NormRasterData();
   rdata->setPoints( rx, ry, rz, plt_zmin, plt_zmax, drect );
   d_spectrogram->setData( rdata );
   d_spectrogram->setColorMap( ColorMapCopy( colormap ) );

   d_spectrogram->attach( data_plot );

   // Mark the grid points NNLS will drop outright
   if ( have_cut  &&  ck_cutmark->isChecked() )
   {
      QVector< double > cx;
      QVector< double > cy;

      for ( int ii = 0; ii < xy_distro.size(); ii++ )
      {
         if ( gnorm[ ii ] < ginfo.norm_cut )
         {
            cx << xy_distro[ ii ].s;
            cy << xy_distro[ ii ].k;
         }
      }

      if ( cx.size() > 0 )
      {
         QwtPlotCurve* ccurve = new QwtPlotCurve(
            tr( "Dropped by NNLS (norm < %1)" )
               .arg( ginfo.norm_cut, 0, 'g', 4 ) );
         ccurve->setStyle ( QwtPlotCurve::NoCurve );
         ccurve->setSymbol( new QwtSymbol( QwtSymbol::XCross,
            QBrush(), QPen( Qt::darkGreen, 1 ), QSize( 5, 5 ) ) );
         ccurve->setSamples( cx, cy );
         ccurve->attach( data_plot );
      }
   }

   // Mark the grid point the cross sections are taken through
   if ( have_grid )
   {
      int jj      = pick_iy * ginfo.nss + pick_ix;

      if ( jj >= 0  &&  jj < xy_distro.size() )
      {
         QwtPlotMarker* pmark = new QwtPlotMarker();
         pmark->setLineStyle ( QwtPlotMarker::Cross );
         pmark->setLinePen   ( QPen( Qt::darkGray, 1, Qt::DashLine ) );
         pmark->setValue     ( xy_distro[ jj ].s, xy_distro[ jj ].k );
         pmark->attach       ( data_plot );
      }
   }

   data_plot->setAxisScale( QwtPlot::xBottom, xmin, xmax );
   data_plot->setAxisScale( QwtPlot::yLeft,   ymin, ymax );

   QwtScaleWidget* rightAxis = data_plot->axisWidget( QwtPlot::yRight );
   rightAxis->setColorBarEnabled( true );
   rightAxis->setColorMap( QwtInterval( plt_zmin, plt_zmax ),
                           ColorMapCopy( colormap ) );
   data_plot->setAxisScale( QwtPlot::yRight, plt_zmin, plt_zmax );

   data_plot->replot();

   update_stats();
   plot_sections();
}

void US_show_norm::plot_data( int )
{
   plot_data();
}

// Redraw the two cross-section plots.
//
// These are the direct answer to "does f/f0 do anything here".  On the map
// a two percent variation along Y is invisible next to a norm that falls to
// zero along X; on a curve scaled to its own range it is unmistakable.
void US_show_norm::plot_sections( void )
{
   xsec_plot_y->detachItems( QwtPlotItem::Rtti_PlotCurve  );
   xsec_plot_y->detachItems( QwtPlotItem::Rtti_PlotMarker );
   xsec_plot_x->detachItems( QwtPlotItem::Rtti_PlotCurve  );
   xsec_plot_x->detachItems( QwtPlotItem::Rtti_PlotMarker );

   if ( ! have_grid  ||  xy_distro.size() < 1 )
   {
      xsec_plot_y->setTitle( tr( "Section along Y (grid shape unknown)" ) );
      xsec_plot_x->setTitle( tr( "Section along X (grid shape unknown)" ) );
      xsec_plot_y->replot();
      xsec_plot_x->replot();
      return;
   }

   int nss     = ginfo.nss;
   int nks     = ginfo.nks;
   pick_ix     = qBound( 0, pick_ix, nss - 1 );
   pick_iy     = qBound( 0, pick_iy, nks - 1 );

   QString ztitle = zmode_title();

   // Section along Y at the selected X
   QVector< double > yx;
   QVector< double > yz;

   for ( int iy = 0; iy < nks; iy++ )
   {  // Points the current Z mode has no real value for are left out
      //  rather than drawn at a substituted level
      int jj      = iy * nss + pick_ix;

      if ( ! zvalid[ jj ] )
         continue;

      yx << xy_distro[ jj ].k;
      yz << zvals   [ jj ];
   }

   QwtPlotCurve* ycurve = new QwtPlotCurve( tr( "Section along Y" ) );
   ycurve->setPen    ( QPen( Qt::blue, 2 ) );
   ycurve->setSymbol ( new QwtSymbol( QwtSymbol::Ellipse,
      QBrush( Qt::blue ), QPen( Qt::blue, 1 ), QSize( 4, 4 ) ) );
   ycurve->setSamples( yx, yz );
   ycurve->attach    ( xsec_plot_y );

   xsec_plot_y->setTitle( tr( "Section along Y at %1 = %2" )
      .arg( attr_label( plot_x ) )
      .arg( xy_distro[ pick_iy * nss + pick_ix ].s, 0, 'g', 5 ) );
   xsec_plot_y->setAxisTitle( QwtPlot::xBottom, anno_title( plot_y ) );
   xsec_plot_y->setAxisTitle( QwtPlot::yLeft,   ztitle );
   xsec_plot_y->setAxisAutoScale( QwtPlot::xBottom );
   xsec_plot_y->setAxisAutoScale( QwtPlot::yLeft   );

   // Section along X at the selected Y
   QVector< double > xx;
   QVector< double > xz;

   for ( int ix = 0; ix < nss; ix++ )
   {
      int jj      = pick_iy * nss + ix;

      if ( ! zvalid[ jj ] )
         continue;

      xx << xy_distro[ jj ].s;
      xz << zvals   [ jj ];
   }

   QwtPlotCurve* xcurve = new QwtPlotCurve( tr( "Section along X" ) );
   xcurve->setPen    ( QPen( Qt::darkRed, 2 ) );
   xcurve->setSymbol ( new QwtSymbol( QwtSymbol::Ellipse,
      QBrush( Qt::darkRed ), QPen( Qt::darkRed, 1 ), QSize( 4, 4 ) ) );
   xcurve->setSamples( xx, xz );
   xcurve->attach    ( xsec_plot_x );

   xsec_plot_x->setTitle( tr( "Section along X at %1 = %2" )
      .arg( attr_label( plot_y ) )
      .arg( xy_distro[ pick_iy * nss + pick_ix ].k, 0, 'g', 5 ) );
   xsec_plot_x->setAxisTitle( QwtPlot::xBottom, anno_title( plot_x ) );
   xsec_plot_x->setAxisTitle( QwtPlot::yLeft,   ztitle );
   xsec_plot_x->setAxisAutoScale( QwtPlot::xBottom );
   xsec_plot_x->setAxisAutoScale( QwtPlot::yLeft   );

   // Mark where the two sections cross, i.e. the selected grid point
   for ( int ii = 0; ii < 2; ii++ )
   {
      QwtPlotMarker* pmark = new QwtPlotMarker();
      pmark->setLineStyle( QwtPlotMarker::VLine );
      pmark->setLinePen  ( QPen( Qt::darkGray, 1, Qt::DashLine ) );
      pmark->setXValue   ( ( ii == 0 )
                           ? xy_distro[ pick_iy * nss + pick_ix ].k
                           : xy_distro[ pick_iy * nss + pick_ix ].s );
      pmark->attach      ( ( ii == 0 ) ? xsec_plot_y : xsec_plot_x );
   }

   // Where the Z axis is still a norm, show the NNLS cutoff on it
   if ( have_cut  &&  ( zmode == ZM_NORM  ||  zmode == ZM_RMS  ||
                        zmode == ZM_LOGNORM ) )
   {
      double cutv = ( zmode == ZM_LOGNORM ) ? log10( ginfo.norm_cut )
                  : ( zmode == ZM_RMS     ) ? ( ginfo.norm_cut * rms_scale )
                                            : ginfo.norm_cut;

      for ( int ii = 0; ii < 2; ii++ )
      {
         QwtPlotMarker* cmark = new QwtPlotMarker();
         cmark->setLineStyle( QwtPlotMarker::HLine );
         cmark->setLinePen  ( QPen( Qt::darkGreen, 1, Qt::DashLine ) );
         cmark->setYValue   ( cutv );
         cmark->setLabel    ( QwtText( tr( "NNLS cutoff" ) ) );
         cmark->setLabelAlignment( Qt::AlignRight | Qt::AlignTop );
         cmark->attach      ( ( ii == 0 ) ? xsec_plot_y : xsec_plot_x );
      }
   }

   xsec_plot_y->replot();
   xsec_plot_x->replot();
}

// Number of grid points whose norm falls below the NNLS cutoff
int US_show_norm::count_cut( void )
{
   int kcut    = 0;

   if ( ! have_cut )
      return kcut;

   for ( int ii = 0; ii < gnorm.size(); ii++ )
   {
      if ( gnorm[ ii ] < ginfo.norm_cut )
         kcut++;
   }

   return kcut;
}

// Refresh the summary statistics box.
//
// The numbers here are the point of the window:  they say in one place how
// much of the grid can carry signal at all, how much of the norm variation
// belongs to each axis, and how nearly parallel neighbouring columns are.
void US_show_norm::update_stats( void )
{
   int nsol       = gnorm.size();

   if ( nsol < 1 )
   {
      te_distr_info->setText( tr( "No grid points." ) );
      return;
   }

   QVector< double > sorted = gnorm;
   std::sort( sorted.begin(), sorted.end() );

   double nmin    = sorted.first();
   double nmax    = sorted.last();
   double nmed    = sorted[ nsol / 2 ];
   int    kcut    = count_cut();

   QString txt    = ginfo.descr.isEmpty()
                  ? QString() : ( ginfo.descr + "\n" );

   txt           += tr( "Grid points          : %1\n" ).arg( nsol );
   txt           += tr( "Column norm ||a||    : min %1  median %2  max %3\n" )
                       .arg( nmin, 0, 'g', 4 ).arg( nmed, 0, 'g', 4 )
                       .arg( nmax, 0, 'g', 4 );

   if ( have_rms )
   {  // The same norms per data point, which is what an OD reading means
      txt           += tr( "  as RMS signal (OD)  : min %1  median %2"
                           "  max %3\n" )
                          .arg( nmin * rms_scale, 0, 'g', 4 )
                          .arg( nmed * rms_scale, 0, 'g', 4 )
                          .arg( nmax * rms_scale, 0, 'g', 4 );
      txt           += tr( "  ( ||a|| / sqrt(%1 data points),"
                           " per 1 OD of species )\n" ).arg( ginfo.ndpts );
   }

   txt           += tr( "Dynamic range max/min: %1\n" )
                       .arg( ( nmin > 0.0 ) ? QString::number( nmax / nmin,
                                                               'g', 4 )
                                            : tr( "infinite (min is 0)" ) );

   if ( have_cut )
   {
      txt           += tr( "Below NNLS cutoff %1 : %2 points (%3 %)\n" )
                          .arg( ginfo.norm_cut, 0, 'g', 4 ).arg( kcut )
                          .arg( kcut * 100.0 / (double)nsol, 0, 'f', 1 );

      if ( have_rms )
         txt           += tr( "  cutoff as RMS signal: %1 OD\n" )
                             .arg( ginfo.norm_cut * rms_scale, 0, 'g', 4 );
   }

   if ( have_grid )
   {  // Relative spread of the norm along each grid axis.  The contrast
      //  between the two is the answer to "does f/f0 affect the norm".
      int nss        = ginfo.nss;
      int nks        = ginfo.nks;
      QVector< double > sensy;
      QVector< double > sensx;

      for ( int ix = 0; ix < nss; ix++ )
      {
         double vlo     = 1.0e+30;
         double vhi     = -1.0e+30;
         double vsum    = 0.0;

         for ( int iy = 0; iy < nks; iy++ )
         {
            double zval    = gnorm[ iy * nss + ix ];
            vlo            = qMin( vlo, zval );
            vhi            = qMax( vhi, zval );
            vsum          += zval;
         }

         double vmean   = vsum / (double)nks;

         if ( vmean > 0.0 )
            sensy << ( ( vhi - vlo ) * 100.0 / vmean );
      }

      for ( int iy = 0; iy < nks; iy++ )
      {
         double vlo     = 1.0e+30;
         double vhi     = -1.0e+30;
         double vsum    = 0.0;

         for ( int ix = 0; ix < nss; ix++ )
         {
            double zval    = gnorm[ iy * nss + ix ];
            vlo            = qMin( vlo, zval );
            vhi            = qMax( vhi, zval );
            vsum          += zval;
         }

         double vmean   = vsum / (double)nss;

         if ( vmean > 0.0 )
            sensx << ( ( vhi - vlo ) * 100.0 / vmean );
      }

      std::sort( sensy.begin(), sensy.end() );
      std::sort( sensx.begin(), sensx.end() );

      txt           += tr( "\nNorm spread across the grid"
                           " (median of (max-min)/mean):\n" );
      txt           += tr( "   along %1 : %2 %\n" )
                          .arg( attr_label( plot_y ),
                                -8 )
                          .arg( sensy.isEmpty() ? 0.0
                                : sensy[ sensy.size() / 2 ], 0, 'f', 2 );
      txt           += tr( "   along %1 : %2 %\n" )
                          .arg( attr_label( plot_x ),
                                -8 )
                          .arg( sensx.isEmpty() ? 0.0
                                : sensx[ sensx.size() / 2 ], 0, 'f', 2 );
   }

   if ( have_sigs )
   {  // How far the selected point has to be moved before the data can
      //  tell the difference.  This is the resolution of the experiment at
      //  that point, and the number a peak position should be quoted with.
      const double cthr = 0.99;      // still separable below this cosine
      int    nss     = ginfo.nss;
      int    nks     = ginfo.nks;
      int    ixp     = qBound( 0, pick_ix, nss - 1 );
      int    iyp     = qBound( 0, pick_iy, nks - 1 );
      int    kpick   = iyp * nss + ixp;
      int    ixlo    = ixp;
      int    ixhi    = ixp;
      int    iylo    = iyp;
      int    iyhi    = iyp;

      while ( ixlo > 0  &&
              coher_pair( kpick, iyp * nss + ixlo - 1 ) >= cthr )  ixlo--;
      while ( ixhi < ( nss - 1 )  &&
              coher_pair( kpick, iyp * nss + ixhi + 1 ) >= cthr )  ixhi++;
      while ( iylo > 0  &&
              coher_pair( kpick, ( iylo - 1 ) * nss + ixp ) >= cthr )  iylo--;
      while ( iyhi < ( nks - 1 )  &&
              coher_pair( kpick, ( iyhi + 1 ) * nss + ixp ) >= cthr )  iyhi++;

      txt           += tr( "\nResolution around the selected point"
                           " (|cos| < %1 to separate):\n" )
                          .arg( cthr, 0, 'g', 3 );
      txt           += tr( "   along %1 : %2 of %3 grid steps,  %4 to %5\n" )
                          .arg( attr_label( plot_x ), -8 )
                          .arg( ixhi - ixlo + 1 ).arg( nss )
                          .arg( attr_value( iyp * nss + ixlo, plot_x ),
                                0, 'g', 4 )
                          .arg( attr_value( iyp * nss + ixhi, plot_x ),
                                0, 'g', 4 );
      txt           += tr( "   along %1 : %2 of %3 grid steps,  %4 to %5\n" )
                          .arg( attr_label( plot_y ), -8 )
                          .arg( iyhi - iylo + 1 ).arg( nks )
                          .arg( attr_value( iylo * nss + ixp, plot_y ),
                                0, 'g', 4 )
                          .arg( attr_value( iyhi * nss + ixp, plot_y ),
                                0, 'g', 4 );
      txt           += tr( "   Grid points inside that span cannot be told\n"
                           "   apart from the selected one by these data.\n" );
   }

   if ( have_coher )
   {  // Median collinearity along each grid direction
      QVector< double > cohx;
      QVector< double > cohy;

      for ( int ii = 0; ii < nsol; ii++ )
      {
         if ( ginfo.coher_x[ ii ] >= 0.0 )  cohx << ginfo.coher_x[ ii ];
         if ( ginfo.coher_y[ ii ] >= 0.0 )  cohy << ginfo.coher_y[ ii ];
      }

      std::sort( cohx.begin(), cohx.end() );
      std::sort( cohy.begin(), cohy.end() );

      txt           += tr( "\nNeighbour collinearity |cos| (median):\n" );
      txt           += tr( "   X direction : %1\n" )
                          .arg( cohx.isEmpty() ? 0.0
                                : cohx[ cohx.size() / 2 ], 0, 'f', 6 );
      txt           += tr( "   Y direction : %1\n" )
                          .arg( cohy.isEmpty() ? 0.0
                                : cohy[ cohy.size() / 2 ], 0, 'f', 6 );
      txt           += tr( "   Values near 1 mean NNLS cannot separate the\n"
                           "   two columns:  their amplitude split is set\n"
                           "   by noise, not by the data.\n" );
   }

   else
      txt           += tr( "\nNeighbour collinearity not available for"
                           " this grid.\n" );

   te_distr_info->setText( txt );
}

// Grid index of the point nearest a plot position, or -1
int US_show_norm::nearest_point( const QPointF& pos )
{
   int    knear   = -1;
   double dnear   = 1.0e+30;
   double xrng    = qMax( plt_smax - plt_smin, 1.0e-30 );
   double yrng    = qMax( plt_kmax - plt_kmin, 1.0e-30 );

   for ( int ii = 0; ii < xy_distro.size(); ii++ )
   {  // Compare in axis-normalized units, since X and Y may differ by many
      //  orders of magnitude depending on the attributes selected
      double dx      = ( pos.x() - xy_distro[ ii ].s ) / xrng;
      double dy      = ( pos.y() - xy_distro[ ii ].k ) / yrng;
      double dsq     = ( dx * dx ) + ( dy * dy );

      if ( dsq < dnear )
      {
         dnear          = dsq;
         knear          = ii;
      }
   }

   return knear;
}

// Handle a click in the map:  move the cross sections to that grid point
void US_show_norm::pick_point( const QPointF& pos )
{
   int knear      = nearest_point( pos );

   if ( knear < 0 )
      return;

   if ( have_grid )
   {
      pick_ix        = knear % ginfo.nss;
      pick_iy        = knear / ginfo.nss;
   }

   QString ptxt   = tr( "%1 %2,  %3 %4,  norm %5" )
      .arg( attr_label( plot_x ) )
      .arg( xy_distro[ knear ].s, 0, 'g', 5 )
      .arg( attr_label( plot_y ) )
      .arg( xy_distro[ knear ].k, 0, 'g', 5 )
      .arg( gnorm[ knear ], 0, 'g', 5 );

   if ( have_rms )
      ptxt          += tr( " (%1 OD RMS)" )
         .arg( gnorm[ knear ] * rms_scale, 0, 'g', 4 );

   if ( have_coher )
      ptxt          += tr( ",  cos to X nb %1,  to Y nb %2" )
         .arg( ginfo.coher_x[ knear ], 0, 'f', 6 )
         .arg( ginfo.coher_y[ knear ], 0, 'f', 6 );

   if ( have_cut  &&  gnorm[ knear ] < ginfo.norm_cut )
      ptxt          += tr( "   [dropped by NNLS]" );

   le_pickinfo->setText( ptxt );

   plot_data();
}

void US_show_norm::select_zmode( int ival )
{
   zmode    = ival;

   plot_data();
}

void US_show_norm::select_cutmark()
{
   plot_data();
}

void US_show_norm::update_plot_smin( double dval )
{
   plt_smin = dval;
}

void US_show_norm::update_plot_smax( double dval )
{
   plt_smax = dval;
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
   plot_data();
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

// Derive the plot limits from the points that will be drawn
void US_show_norm::set_limits()
{
   double smin = 1.0e30;
   double smax = -1.0e30;
   double kmin = 1.0e30;
   double kmax = -1.0e30;

   xa_title    = anno_title( plot_x );
   ya_title    = anno_title( plot_y );

   data_plot->setAxisTitle( QwtPlot::xBottom, xa_title );
   data_plot->setAxisTitle( QwtPlot::yLeft,   ya_title );

   int nsol    = model->components.count();

   if ( nsol < 1 )
      return;

   for ( int ii = 0; ii < nsol; ii++ )
   {
      double sval = attr_value( ii, plot_x );
      double kval = attr_value( ii, plot_y );
      smin        = qMin( smin, sval );
      smax        = qMax( smax, sval );
      kmin        = qMin( kmin, kval );
      kmax        = qMax( kmax, kval );
   }

   double sinc = ( smax > smin ) ? ( ( smax - smin ) * 0.02 )
                                 : qMax( qAbs( smax ) * 0.05, 1.0e-6 );
   double kinc = ( kmax > kmin ) ? ( ( kmax - kmin ) * 0.02 )
                                 : qMax( qAbs( kmax ) * 0.05, 1.0e-6 );

   if ( auto_sxy )
   {  // Set auto limits on X and Y
      plt_smin    = smin - sinc;
      plt_smax    = smax + sinc;
      plt_kmin    = kmin - kinc;
      plt_kmax    = kmax + kinc;

      ct_plt_smin->setValue( plt_smin );
      ct_plt_smax->setValue( plt_smax );
      ct_plt_kmin->setValue( plt_kmin );
      ct_plt_kmax->setValue( plt_kmax );
   }

   else
   {
      plt_smin    = ct_plt_smin->value();
      plt_smax    = ct_plt_smax->value();
      plt_kmin    = ct_plt_kmin->value();
      plt_kmax    = ct_plt_kmax->value();
   }
}

// Check and enable the axis radio buttons to match plot_x and plot_y
void US_show_norm::sync_axis_buttons( void )
{
   QAbstractButton* bx = bg_x_axis->button( plot_x );
   QAbstractButton* by = bg_y_axis->button( plot_y );

   if ( bx != NULL )  bx->setChecked( true );
   if ( by != NULL )  by->setChecked( true );

   // An attribute already used on one axis cannot be chosen for the other
   rb_x_s   ->setEnabled( plot_y != ATTR_S );
   rb_x_ff0 ->setEnabled( plot_y != ATTR_K );
   rb_x_mw  ->setEnabled( plot_y != ATTR_W );
   rb_x_vbar->setEnabled( plot_y != ATTR_V );
   rb_x_D   ->setEnabled( plot_y != ATTR_D );
   rb_x_f   ->setEnabled( plot_y != ATTR_F );
   rb_y_s   ->setEnabled( plot_x != ATTR_S );
   rb_y_ff0 ->setEnabled( plot_x != ATTR_K );
   rb_y_mw  ->setEnabled( plot_x != ATTR_W );
   rb_y_vbar->setEnabled( plot_x != ATTR_V );
   rb_y_D   ->setEnabled( plot_x != ATTR_D );
   rb_y_f   ->setEnabled( plot_x != ATTR_F );
}

// Apply the limit labels and counter ranges of the current axis attributes
void US_show_norm::apply_axis_ranges( void )
{
   const QString labs[] = {      "s", "f/f0",  "MW", "vbar",   "D",    "f" };
   const double  mins[] = { -10000.0,   0.0,    0.0,   0.00,   0.0,    0.0 };
   const double  maxs[] = {  10000.0,  50.0,  1e+10,   3.00,  1e+5,   1e+5 };
   const double  incs[] = {     0.01,  0.01, 1000.0,   0.01,  0.01,   1e-9 };

   lb_plt_smin->setText( tr( "Plot Limit " ) + labs[ plot_x ]
                       + tr( " Minimum:" ) );
   lb_plt_smax->setText( tr( "Plot Limit " ) + labs[ plot_x ]
                       + tr( " Maximum:" ) );
   ct_plt_smin->setRange( mins[ plot_x ], maxs[ plot_x ] );
   ct_plt_smax->setRange( mins[ plot_x ], maxs[ plot_x ] );
   ct_plt_smin->setSingleStep( incs[ plot_x ] );
   ct_plt_smax->setSingleStep( incs[ plot_x ] );

   lb_plt_kmin->setText( tr( "Plot Limit " ) + labs[ plot_y ]
                       + tr( " Minimum:" ) );
   lb_plt_kmax->setText( tr( "Plot Limit " ) + labs[ plot_y ]
                       + tr( " Maximum:" ) );
   ct_plt_kmin->setRange( mins[ plot_y ], maxs[ plot_y ] );
   ct_plt_kmax->setRange( mins[ plot_y ], maxs[ plot_y ] );
   ct_plt_kmin->setSingleStep( incs[ plot_y ] );
   ct_plt_kmax->setSingleStep( incs[ plot_y ] );
}

// Select coordinate for horizontal axis
void US_show_norm::select_x_axis( int ival )
{
   plot_x     = ival;

   sync_axis_buttons();
   apply_axis_ranges();
   set_limits();
   plot_data();
}

// Select coordinate for vertical axis
void US_show_norm::select_y_axis( int ival )
{
   plot_y     = ival;

   sync_axis_buttons();
   apply_axis_ranges();
   set_limits();
   plot_data();
}

// Set annotation title for a plot index
QString US_show_norm::anno_title( int pltndx )
{
   QString a_title;

   if      ( pltndx == ATTR_S )
      a_title  = tr( "s (1e-13) for water at 20" ) + DEGC;
   else if ( pltndx == ATTR_K )
      a_title  = tr( "f/f0 (Frictional Ratio)" );
   else if ( pltndx == ATTR_W )
      a_title  = tr( "MW (Dalton)" );
   else if ( pltndx == ATTR_V )
      a_title  = tr( "vbar at 20" ) + DEGC;
   else if ( pltndx == ATTR_D )
      a_title  = tr( "D (1e-7)" );
   else if ( pltndx == ATTR_F )
      a_title  = tr( "f (Frictional Coefficient)" );

   return a_title;
}

// Make a ColorMap copy and return a pointer to the new ColorMap
QwtLinearColorMap* US_show_norm::ColorMapCopy( QwtLinearColorMap* colormap )
{
   QVector< double >  cstops   = colormap->colorStops();
   int                lstop    = cstops.count() - 1;
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

