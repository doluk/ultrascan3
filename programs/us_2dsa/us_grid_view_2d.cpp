//! \file us_grid_view_2d.cpp
#include <QApplication>

#include "us_grid_view_2d.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_model.h"
#include "us_math2.h"
#include "qwt_picker_machine.h"
#include "qwt_legend.h"

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

   // Grid information, fit result and subgrid controls on the left side
   QLabel* lb_info         = us_banner( tr( "Grid Information" ) );
   QLabel* lb_desc         = us_label(  tr( "Grid Type:" ) );
   QLabel* lb_npoints      = us_label(  tr( "Total Grid Points:" ) );
   QLabel* lb_nsubgrids    = us_label(  tr( "Number of Subgrids:" ) );
   QLabel* lb_fitres       = us_banner( tr( "Fit Results" ) );
   QLabel* lb_iter         = us_label(  tr( "Refinement Iteration:" ) );
   QLabel* lb_iter_rmsd    = us_label(  tr( "Iteration Final Fit:" ) );
   QLabel* lb_stage        = us_label(  tr( "Fit Stage:" ) );
   QLabel* lb_subgrid_ctrl = us_banner( tr( "Subgrid / Task Control" ) );
           lb_subgrid      = us_label(  tr( "Subgrid:" ) );
           lb_npoints_curr = us_label(  tr( "Points in Subgrid:" ) );
   QLabel* lb_task_rmsd    = us_label(  tr( "Task Fit RMSD:" ) );
   QLabel* lb_overlay      = us_banner( tr( "Solute Overlays" ) );
   QLabel* lb_nearest      = us_label(  tr( "Clicked:" ) );
   QLabel* lb_optimal      = us_banner( tr( "Final Fit Optimality" ) );
   pb_optcheck             = us_pushbutton( tr( "Check Final Fit" ) );
   te_optinfo              = us_textedit();
   us_setReadOnly( te_optinfo, true );
   te_optinfo->setMinimumHeight( fontMetrics().lineSpacing() * 8 );
   te_optinfo->setText( tr( "Tests whether any grid point could still lower "
                            "the RMSD of the last completed final fit." ) );
   QGridLayout* lo_optmap  =
      us_checkbox( tr( "Improvement Map" ), ck_optmap, true );
   opt_ssq                 = 0.0;
   opt_iter                = -1;
   QLabel* lb_hint         = us_label(
      tr( "Click on a grid point to select its subgrid;"
          " the nearest point or solute is described above." ) );
   lb_hint->setWordWrap( true );

   le_desc         = us_lineedit( "", -1, true );
   le_npoints      = us_lineedit( "", -1, true );
   le_nsubgrids    = us_lineedit( "", -1, true );
   le_npoints_curr = us_lineedit( "", -1, true );
   le_task_rmsd    = us_lineedit( "", -1, true );
   le_iter_rmsd    = us_lineedit( "", -1, true );
   le_nearest      = us_lineedit( "", -1, true );

   QGridLayout* lo_fitres    =
      us_checkbox( tr( "Show Fit Results" ),        ck_fitres,    false );
   QGridLayout* lo_highlight =
      us_checkbox( tr( "Highlight Input" ),         ck_highlight, true  );
   QGridLayout* lo_colorall  =
      us_checkbox( tr( "Color by Subgrid / Task" ), ck_colorall,  false );
   QGridLayout* lo_tasksols  =
      us_checkbox( tr( "Selected Task Solutes" ),   ck_tasksols,  true  );
   QGridLayout* lo_allsols   =
      us_checkbox( tr( "All Tasks' Solutes" ),      ck_allsols,   false );
   QGridLayout* lo_finalsols =
      us_checkbox( tr( "Final Fit Solutes" ),       ck_finalsols, true  );
   QGridLayout* lo_addsols   =
      us_checkbox( tr( "Refinement-Added Points" ), ck_addsols,   true  );
   QGridLayout* lo_poolsols  =
      us_checkbox( tr( "Previous Depth Solutes" ),  ck_poolsols,  true  );
   QGridLayout* lo_origgrid  =
      us_checkbox( tr( "Original Grid" ),           ck_origgrid,  true  );
   ck_fitres->setEnabled( false );

   cb_stage        = us_comboBox();
   cb_stage->addItem( tr( "Subgrids (depth 0)" ), 0 );

   ct_iter         = us_counter( 2, 1, 1, 1 );
   ct_iter   ->setSingleStep( 1 );
   ct_subgrid      = us_counter( 2, 1, 1, 1 );
   ct_subgrid->setSingleStep( 1 );
   QLineEdit* le_sg = ct_subgrid->findChild< QLineEdit* >();
   if ( le_sg )
      le_sg->setAlignment( Qt::AlignCenter );
   QLineEdit* le_it = ct_iter->findChild< QLineEdit* >();
   if ( le_it )
      le_it->setAlignment( Qt::AlignCenter );

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
   left->addWidget( lb_fitres,        row++, 0, 1, 4 );
   left->addLayout( lo_fitres,        row++, 0, 1, 4 );
   left->addWidget( lb_iter,          row,   0, 1, 2 );
   left->addWidget( ct_iter,          row++, 2, 1, 2 );
   left->addWidget( lb_iter_rmsd,     row,   0, 1, 2 );
   left->addWidget( le_iter_rmsd,     row++, 2, 1, 2 );
   left->addWidget( lb_stage,         row,   0, 1, 2 );
   left->addWidget( cb_stage,         row++, 2, 1, 2 );
   left->addWidget( lb_subgrid_ctrl,  row++, 0, 1, 4 );
   left->addLayout( lo_highlight,     row,   0, 1, 2 );
   left->addLayout( lo_colorall,      row++, 2, 1, 2 );
   left->addWidget( lb_subgrid,       row,   0, 1, 2 );
   left->addWidget( ct_subgrid,       row++, 2, 1, 2 );
   left->addWidget( lb_npoints_curr,  row,   0, 1, 2 );
   left->addWidget( le_npoints_curr,  row++, 2, 1, 2 );
   left->addWidget( lb_task_rmsd,     row,   0, 1, 2 );
   left->addWidget( le_task_rmsd,     row++, 2, 1, 2 );
   left->addWidget( lb_overlay,       row++, 0, 1, 4 );
   left->addLayout( lo_tasksols,      row,   0, 1, 2 );
   left->addLayout( lo_allsols,       row++, 2, 1, 2 );
   left->addLayout( lo_finalsols,     row,   0, 1, 2 );
   left->addLayout( lo_addsols,       row++, 2, 1, 2 );
   left->addLayout( lo_poolsols,      row,   0, 1, 2 );
   left->addLayout( lo_origgrid,      row++, 2, 1, 2 );
   left->addWidget( lb_nearest,       row,   0, 1, 1 );
   left->addWidget( le_nearest,       row++, 1, 1, 3 );
   left->addWidget( lb_hint,          row++, 0, 1, 4 );
   left->addWidget( lb_optimal,       row++, 0, 1, 4 );
   left->addWidget( pb_optcheck,      row,   0, 1, 2 );
   left->addLayout( lo_optmap,        row++, 2, 1, 2 );
   left->addWidget( te_optinfo,       row++, 0, 1, 4 );
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
   data_plot->insertLegend( new QwtLegend(), QwtPlot::BottomLegend );

   color_base      = QColor( 0,    47, 167 );
   color_subgrid   = QColor( 173, 255,  47 );
   color_tasksol   = QColor( 255,  69,   0 );
   color_final     = QColor( 255,   0, 255 );
   color_added     = QColor( 0,   229, 255 );
   color_pool      = QColor( 220, 220, 220 );
   hl_curve        = point_curve( tr( "Selected Input" ), true );
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
   connect( ct_iter,      &QwtCounter::valueChanged,
            this,         &US_GridView2D::iter_changed );
   connect( cb_stage,     QOverload< int >::of( &QComboBox::currentIndexChanged ),
            this,         &US_GridView2D::stage_changed );
   connect( ck_fitres,    &QCheckBox::toggled,
            this,         &US_GridView2D::fitres_toggled );
   connect( ck_highlight, &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( ck_colorall,  &QCheckBox::toggled,
            this,         &US_GridView2D::plot_points );
   connect( ck_tasksols,  &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( ck_allsols,   &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( ck_finalsols, &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( ck_poolsols,  &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( ck_origgrid,  &QCheckBox::toggled,
            this,         &US_GridView2D::plot_points );
   connect( ck_addsols,   &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( pick,         QOverload< const QPointF& >::of( &QwtPlotPicker::selected ),
            this,         &US_GridView2D::point_clicked );
   connect( pb_optcheck,  &QPushButton::clicked,
            this,         &US_GridView2D::optimality_requested );
   connect( ck_optmap,    &QCheckBox::toggled,
            this,         &US_GridView2D::plot_subgrid );
   connect( pb_help,      &QPushButton::clicked,
            this,         &US_GridView2D::help );
   connect( pb_close,     &QPushButton::clicked,
            this,         &US_GridView2D::close );

   set_grid( subgrids, desc );
}

// Replace the grid from current settings
void US_GridView2D::set_grid( const QList< QVector< US_Solute > >& subgrids,
                              const QString& desc )
{
   sgrid_sols      = subgrids;
   sgrid_desc      = desc;

   // For a varying-vbar grid, default the Y axis to vbar
   bool cnst_k     = true;
   bool cnst_v     = true;
   int  npoints    = 0;
   double k0       = 0.0;
   double v0       = 0.0;

   for ( int ii = 0; ii < subgrids.size(); ii++ )
   {
      for ( int jj = 0; jj < subgrids[ ii ].size(); jj++ )
      {
         const US_Solute& sol = subgrids[ ii ][ jj ];

         if ( npoints == 0 )
         {
            k0             = sol.k;
            v0             = sol.v;
         }
         cnst_k         = cnst_k  &&  ( sol.k == k0 );
         cnst_v         = cnst_v  &&  ( sol.v == v0 );
         npoints++;
      }
   }

   if ( npoints > 0  &&  cnst_k  &&  ! cnst_v )
      y_axis->button( ATTR_V )->setChecked( true );
   else if ( y_axis->checkedId() == ATTR_V  &&  cnst_v )
      y_axis->button( ATTR_K )->setChecked( true );

   if ( ! show_fit() )
      refresh_all();
}

// Replace the fit task records
void US_GridView2D::set_fit_records( const QList< US_2dsaTaskRecord >& records )
{
   bool had_recs   = ! recs.isEmpty();
   bool at_last    = ( (int)ct_iter->value() >= iter_count() );
   recs            = records;
   int  niters     = qMax( 1, iter_count() );

   {
      QSignalBlocker sbfit( ck_fitres );
      QSignalBlocker sbitr( ct_iter );
      ck_fitres->setEnabled( ! recs.isEmpty() );

      if ( recs.isEmpty() )
         ck_fitres->setChecked( false );
      else if ( ! had_recs )
         ck_fitres->setChecked( true );    // Show results of a new fit

      ct_iter->setRange( 1, niters );
      if ( at_last  ||  ! had_recs )
         ct_iter->setValue( niters );      // Follow the latest iteration
   }
DbgLv(1) << "GV2D:SFR: nrecs niters" << recs.size() << niters;

   refresh_all();
}

// Flag if fit results are to be shown
bool US_GridView2D::show_fit()
{
   return ( ck_fitres->isChecked()  &&  ! recs.isEmpty() );
}

// Count refinement iterations in the records
int US_GridView2D::iter_count()
{
   int niters      = 0;

   for ( int ii = 0; ii < recs.size(); ii++ )
      niters          = qMax( niters, recs[ ii ].iter + 1 );

   return niters;
}

// Index of the final fit record of an iteration (or -1)
int US_GridView2D::final_rec( int iter )
{
   for ( int ii = recs.size() - 1; ii >= 0; ii-- )
   {
      if ( recs[ ii ].final  &&  recs[ ii ].iter == iter )
         return ii;
   }

   return -1;
}

// Index of the record of the selected task (or -1)
int US_GridView2D::current_rec()
{
   int tskx        = (int)ct_subgrid->value() - 1;

   if ( ! show_fit()  ||  tskx < 0  ||  tskx >= stage_recs.size() )
      return -1;

   return stage_recs[ tskx ];
}

// Rebuild the grid, stage and task lists and replot everything
void US_GridView2D::refresh_all()
{
   bool fitres     = show_fit();
   ct_iter     ->setEnabled( fitres );
   cb_stage    ->setEnabled( fitres );
   ck_tasksols ->setEnabled( fitres );
   ck_allsols  ->setEnabled( fitres );
   ck_finalsols->setEnabled( fitres );
   ck_addsols  ->setEnabled( fitres );
   ck_poolsols ->setEnabled( fitres );

   build_grid();
   fill_stages();
   fill_tasks();
   plot_points();
}

// Build the displayed grid:  settings grid, or grid of a fit iteration
void US_GridView2D::build_grid()
{
   added_sols.clear();
   final_sols.clear();
   le_iter_rmsd->clear();

   if ( ! show_fit() )
   {
      grid_sols       = sgrid_sols;
      le_desc->setText( sgrid_desc );
   }

   else
   {
      int iter        = (int)ct_iter->value() - 1;
      int kfinal      = final_rec( iter - 1 );

      // Solutes added to every subgrid are the previous iteration's result
      if ( iter > 0  &&  kfinal >= 0  &&  recs[ kfinal ].done )
         added_sols      = recs[ kfinal ].csolutes;

      grid_sols.clear();

      for ( int ii = 0; ii < recs.size(); ii++ )
      {
         const US_2dsaTaskRecord& trec = recs[ ii ];

         if ( trec.iter != iter  ||  trec.depth != 0  ||  trec.final )
            continue;

         while ( grid_sols.size() <= trec.taskx )
            grid_sols << QVector< US_Solute >();

         QVector< US_Solute > gsols;

         for ( int jj = 0; jj < trec.isolutes.size(); jj++ )
         {
            if ( ! added_sols.contains( trec.isolutes[ jj ] ) )
               gsols << trec.isolutes[ jj ];
         }

         grid_sols[ trec.taskx ] = gsols;
      }

      kfinal          = final_rec( iter );

      if ( kfinal >= 0  &&  recs[ kfinal ].done )
      {
         final_sols      = recs[ kfinal ].csolutes;
         le_iter_rmsd->setText( tr( "RMSD %1 (%2 solutes)" )
            .arg( sqrt( recs[ kfinal ].variance ), 0, 'e', 4 )
            .arg( final_sols.size() ) );
      }
      else
         le_iter_rmsd->setText( tr( "(in progress)" ) );

      le_desc->setText( tr( "Fit Grid, Iteration %1" ).arg( iter + 1 ) );
   }

   grid.clear();
   int npoints     = 0;

   for ( int ii = 0; ii < grid_sols.size(); ii++ )
   {
      QVector< GridPt > gpts;

      for ( int jj = 0; jj < grid_sols[ ii ].size(); jj++ )
         gpts << make_point( grid_sols[ ii ][ jj ] );

      npoints        += gpts.size();
      grid << gpts;
   }

   le_npoints  ->setText( QString::number( npoints ) );
   le_nsubgrids->setText( QString::number( grid.size() ) );
DbgLv(1) << "GV2D:BG: fit" << show_fit() << "npoints nsubg" << npoints
 << grid.size() << "nadded nfinal" << added_sols.size() << final_sols.size();
}

// Fill the list of fit stages (depths) of the selected iteration
void US_GridView2D::fill_stages()
{
   QSignalBlocker sblock( cb_stage );
   int stage       = cb_stage->currentData().toInt();
   cb_stage->clear();
   cb_stage->addItem( tr( "Subgrids (depth 0)" ), 0 );

   if ( show_fit() )
   {
      int iter        = (int)ct_iter->value() - 1;
      int maxdepth    = 0;

      for ( int ii = 0; ii < recs.size(); ii++ )
      {
         if ( recs[ ii ].iter == iter  &&  ! recs[ ii ].final )
            maxdepth        = qMax( maxdepth, recs[ ii ].depth );
      }

      for ( int dd = 1; dd <= maxdepth; dd++ )
         cb_stage->addItem( tr( "Depth %1 Tasks" ).arg( dd ), dd );

      if ( final_rec( iter ) >= 0 )
         cb_stage->addItem( tr( "Final Fit" ), -1 );
   }

   int stgx        = cb_stage->findData( stage );
   cb_stage->setCurrentIndex( qMax( 0, stgx ) );
}

// Fill the list of task records of the selected stage
void US_GridView2D::fill_tasks()
{
   stage_recs.clear();
   int stage       = cb_stage->currentData().toInt();
   int ntasks      = grid.size();

   if ( show_fit() )
   {
      int iter        = (int)ct_iter->value() - 1;

      for ( int ii = 0; ii < recs.size(); ii++ )
      {
         const US_2dsaTaskRecord& trec = recs[ ii ];

         if ( trec.iter != iter )
            continue;

         if ( ( stage < 0  &&  trec.final )  ||
              ( stage >= 0  &&  ! trec.final  &&  trec.depth == stage ) )
            stage_recs << ii;
      }

      // Depth 0 tasks are in subgrid order
      if ( stage == 0 )
      {
         std::sort( stage_recs.begin(), stage_recs.end(),
                    [ this ]( int a, int b )
                    { return recs[ a ].taskx < recs[ b ].taskx; } );
      }

      ntasks          = stage_recs.size();
   }

   lb_subgrid     ->setText( stage == 0 ? tr( "Subgrid:" ) : tr( "Task:" ) );
   lb_npoints_curr->setText( show_fit() ? tr( "Solutes In / Out:" )
                                        : tr( "Points in Subgrid:" ) );

   QSignalBlocker sblock( ct_subgrid );
   ct_subgrid->setRange( 1, qMax( 1, ntasks ) );
   ct_subgrid->setValue( qMin( (int)ct_subgrid->value(), qMax( 1, ntasks ) ) );
}

// Create a symbols-only curve, with or without a legend entry
QwtPlotCurve* US_GridView2D::point_curve( const QString& title, bool legend )
{
   // The legend attribute must be set before the curve is attached
   QwtPlotCurve* curve = new QwtPlotCurve( title );
   curve->setItemAttribute( QwtPlotItem::Legend, legend );
   curve->setStyle        ( QwtPlotCurve::NoCurve );
   curve->setYAxis        ( QwtPlot::yLeft );
   curve->attach          ( data_plot );
   return curve;
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

// Return a short description of a solute
QString US_GridView2D::sol_text( const US_Solute& sol )
{
   QString text = QString( "s=%1  f/f0=%2  vbar=%3" )
      .arg( sol.s * 1.0e+13, 0, 'g', 5 )
      .arg( sol.k, 0, 'g', 5 ).arg( sol.v, 0, 'g', 4 );

   if ( sol.c > 0.0 )
      text += QString( "  c=%1" ).arg( sol.c, 0, 'g', 4 );

   return text;
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
   // Subgrid colors apply to the grid only when tasks are subgrids
   bool   colorall = ck_colorall->isChecked()  &&
                     ( ! show_fit()  ||  cb_stage->currentData().toInt() == 0 );
   bool   showgrid = ck_origgrid->isChecked();

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

      if ( ! showgrid )
         continue;            // Grid only sets the plot ranges

      QColor color    = colorall ? subgrid_color( ii ) : color_base;
      QwtSymbol* symbol = new QwtSymbol( QwtSymbol::Ellipse, QBrush( color ),
                                         QPen( color, 2 ), QSize( ss, ss ) );
      QwtPlotCurve* curve = point_curve( ii == 0 ? tr( "Grid Points" )
                                            : QString( "GRID_%1" ).arg( ii ),
                                         ( ii == 0 ) );
      curve->setSymbol ( symbol );
      curve->setSamples( xarr.data(), yarr.data(), np );
      curve->setZ      ( 0 );
      point_curves << curve;
   }

   // Include any overlaid solutes in the plot ranges
   QVector< US_Solute > xsols = added_sols + final_sols;

   for ( int ii = 0; ii < xsols.size(); ii++ )
   {
      GridPt gpt      = make_point( xsols[ ii ] );
      px1             = qMin( px1, gpt.vals[ xattr ] );
      px2             = qMax( px2, gpt.vals[ xattr ] );
      py1             = qMin( py1, gpt.vals[ yattr ] );
      py2             = qMax( py2, gpt.vals[ yattr ] );
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

// Plot the highlighted subgrid or task input, then the overlays
void US_GridView2D::plot_subgrid()
{
   int  sgx        = (int)ct_subgrid->value() - 1;
   int  krec       = current_rec();
   bool fitres     = show_fit();
   QVector< US_Solute > hsols;
   le_npoints_curr->clear();
   le_task_rmsd   ->clear();
   ct_subgrid     ->setEnabled( fitres ? ( stage_recs.size() > 1 )
                                       : ( grid.size() > 1 ) );

   if ( fitres )
   {
      if ( krec >= 0 )
      {
         const US_2dsaTaskRecord& trec = recs[ krec ];
         hsols           = trec.isolutes;
         le_npoints_curr->setText( trec.done
            ? QString( "%1 / %2" ).arg( trec.isolutes.size() )
                                  .arg( trec.csolutes.size() )
            : QString( "%1 / ..." ).arg( trec.isolutes.size() ) );
         le_task_rmsd   ->setText( trec.done
            ? QString::number( sqrt( trec.variance ), 'e', 4 )
            : tr( "(in progress)" ) );
      }
   }

   else if ( sgx >= 0  &&  sgx < grid_sols.size() )
   {
      hsols           = grid_sols[ sgx ];
      le_npoints_curr->setText( QString::number( hsols.size() ) );
   }

   if ( ! ck_highlight->isChecked() )
      hsols.clear();

   int xattr       = x_axis->checkedId();
   int yattr       = y_axis->checkedId();
   int np          = hsols.size();
   QVector< double > xarr( np );
   QVector< double > yarr( np );

   for ( int jj = 0; jj < np; jj++ )
   {
      GridPt gpt      = make_point( hsols[ jj ] );
      xarr[ jj ]      = gpt.vals[ xattr ];
      yarr[ jj ]      = gpt.vals[ yattr ];
   }

   int    ss       = (int)ct_size->value() + 2;
   QwtSymbol* symbol = new QwtSymbol( QwtSymbol::Ellipse,
                                      QBrush( color_subgrid ),
                                      QPen( color_subgrid, 2 ),
                                      QSize( ss, ss ) );
   hl_curve->setSymbol ( symbol );
   hl_curve->setSamples( xarr.data(), yarr.data(), np );

   plot_overlays();
   plot_optimality();
   data_plot->replot();
}

// Plot fitted, final and refinement-added solutes over the grid
void US_GridView2D::plot_overlays()
{
   for ( int ii = 0; ii < over_curves.size(); ii++ )
   {
      over_curves[ ii ]->detach();
      delete over_curves[ ii ];
   }
   over_curves.clear();
   over_sols  .clear();
   over_kind  .clear();

   if ( ! show_fit() )
      return;

   if ( ck_addsols->isChecked() )
   {
      add_sol_curves( added_sols, QwtSymbol::Diamond, color_added,
                      false, false, tr( "Refinement-Added Points" ),
                      tr( "Added" ), 2.0 );
   }

   int stage       = cb_stage->currentData().toInt();

   if ( stage != 0  &&  ck_poolsols->isChecked() )
   {  // Inputs of merge (or final) tasks:  the solutes selected by the
      // previous depth, grouped (and optionally colored) by receiving task
      bool colorall   = ck_colorall->isChecked();

      for ( int ii = 0; ii < stage_recs.size(); ii++ )
      {
         const US_2dsaTaskRecord& trec = recs[ stage_recs[ ii ] ];
         QString kind    = ( stage < 0 )
            ? tr( "Final fit input" )
            : tr( "Depth %1 solute -> task %2" ).arg( stage - 1 ).arg( ii + 1 );
         add_sol_curves( trec.isolutes, QwtSymbol::Ellipse,
                         colorall ? subgrid_color( ii ) : color_pool,
                         true, true, tr( "Previous Depth Solutes" ),
                         kind, 0.5, ( ii == 0 ), ii );
      }
   }

   if ( stage >= 0 )
   {  // Task solutes (the final fit's solutes are shown separately)
      QVector< US_Solute > tsols;

      if ( ck_allsols->isChecked() )
      {
         for ( int ii = 0; ii < stage_recs.size(); ii++ )
            tsols += recs[ stage_recs[ ii ] ].csolutes;
      }

      else if ( ck_tasksols->isChecked()  &&  current_rec() >= 0 )
         tsols    = recs[ current_rec() ].csolutes;

      add_sol_curves( tsols, QwtSymbol::Ellipse, color_tasksol,
                      true, true, tr( "Task Fit Solutes" ),
                      tr( "Task solute" ), 3.0 );
   }

   if ( ck_finalsols->isChecked()  ||  stage < 0 )
   {
      add_sol_curves( final_sols, QwtSymbol::Ellipse, color_final,
                      false, true, tr( "Final Fit Solutes" ),
                      tr( "Final solute" ), 4.0 );
   }
}

// Add curves for a set of solutes, sized by relative concentration
void US_GridView2D::add_sol_curves( const QVector< US_Solute >& sols,
      QwtSymbol::Style style, const QColor& color, bool filled, bool scaled,
      const QString& title, const QString& kind, double zval,
      bool legend, int tag )
{
   const int nbins = 5;
   int    nsols    = sols.size();

   if ( nsols == 0 )
      return;

   int    xattr    = x_axis->checkedId();
   int    yattr    = y_axis->checkedId();
   int    ss       = (int)ct_size->value();
   double cmax     = 0.0;

   for ( int ii = 0; ii < nsols; ii++ )
      cmax            = qMax( cmax, sols[ ii ].c );

   // Bin the solutes by concentration; each bin has its own symbol size
   QVector< QVector< double > > xarrs( nbins );
   QVector< QVector< double > > yarrs( nbins );

   for ( int ii = 0; ii < nsols; ii++ )
   {
      GridPt gpt      = make_point( sols[ ii ] );
      int    bin      = nbins - 1;

      if ( scaled  &&  cmax > 0.0 )
      {
         bin             = (int)( sqrt( sols[ ii ].c / cmax ) * nbins );
         bin             = qMax( 0, qMin( nbins - 1, bin ) );
      }

      xarrs[ bin ] << gpt.vals[ xattr ];
      yarrs[ bin ] << gpt.vals[ yattr ];
      over_sols    << sols[ ii ];
      over_kind    << kind;
   }

   for ( int bb = 0; bb < nbins; bb++ )
   {
      int np          = xarrs[ bb ].size();

      if ( np == 0 )
         continue;

      int    sz       = ss + 4 + bb * 3;
      QBrush brush    = filled ? QBrush( color ) : QBrush( Qt::NoBrush );
      QwtSymbol* symbol = new QwtSymbol( style, brush, QPen( color, 2 ),
                                         QSize( sz, sz ) );
      QwtPlotCurve* curve = point_curve( legend ? title
                        : QString( "%1_%2_%3" ).arg( title ).arg( tag ).arg( bb ),
                                         legend );
      curve->setSymbol ( symbol );
      curve->setSamples( xarrs[ bb ].data(), yarrs[ bb ].data(), np );
      curve->setZ      ( zval );
      over_curves << curve;
      legend          = false;
   }
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

// Change the selected subgrid or task
void US_GridView2D::subgrid_changed( double )
{
   plot_subgrid();
}

// Change the displayed refinement iteration
void US_GridView2D::iter_changed( double )
{
   refresh_all();
}

// Change the displayed fit stage
void US_GridView2D::stage_changed( int )
{
   fill_tasks();
   plot_points();          // Grid coloring depends on the stage
}

// Turn display of fit results on or off
void US_GridView2D::fitres_toggled( bool )
{
   refresh_all();
}

// Select the subgrid of the grid point nearest a mouse click, and
// describe the nearest grid point or overlaid solute
void US_GridView2D::point_clicked( const QPointF& pos )
{
   const double maxdist = 10.0;       // Maximum pixel distance
   int    xattr    = x_axis->checkedId();
   int    yattr    = y_axis->checkedId();
   double cx       = data_plot->transform( QwtPlot::xBottom, pos.x() );
   double cy       = data_plot->transform( QwtPlot::yLeft,   pos.y() );
   double mindist  = 1e99;
   int    minsgx   = -1;
   int    minptx   = -1;

   int    ngrid    = ck_origgrid->isChecked() ? grid.size() : 0;

   for ( int ii = 0; ii < ngrid; ii++ )
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
            minptx          = jj;
         }
      }
   }

   // Overlaid solutes take precedence when as near as a grid point
   double minodist = 1e99;
   int    minsolx  = -1;

   for ( int ii = 0; ii < over_sols.size(); ii++ )
   {
      GridPt gpt      = make_point( over_sols[ ii ] );
      double px       = data_plot->transform( QwtPlot::xBottom,
                                              gpt.vals[ xattr ] );
      double py       = data_plot->transform( QwtPlot::yLeft,
                                              gpt.vals[ yattr ] );
      double dist     = sq( px - cx ) + sq( py - cy );

      if ( dist < minodist )
      {
         minodist        = dist;
         minsolx         = ii;
      }
   }

   if ( minsolx >= 0  &&  minodist <= sq( maxdist )  &&  minodist <= mindist )
   {
      le_nearest->setText( over_kind[ minsolx ] + ": "
                           + sol_text( over_sols[ minsolx ] ) );
      le_nearest->setCursorPosition( 0 );
      return;
   }

   if ( minsgx < 0  ||  mindist > sq( maxdist ) )
      return;

   le_nearest->setText( tr( "Grid point (subgrid %1): " ).arg( minsgx + 1 )
                        + sol_text( grid_sols[ minsgx ][ minptx ] ) );
   le_nearest->setCursorPosition( 0 );

   // Select the subgrid only when tasks are subgrids
   if ( show_fit()  &&  cb_stage->currentData().toInt() != 0 )
      return;

   int tskx        = minsgx;

   if ( show_fit() )
   {  // Find the depth 0 task of this subgrid
      tskx            = -1;
      for ( int ii = 0; ii < stage_recs.size(); ii++ )
         if ( recs[ stage_recs[ ii ] ].taskx == minsgx )
            tskx            = ii;
      if ( tskx < 0 )
         return;
   }

   {
      QSignalBlocker sblock( ct_subgrid );
      ct_subgrid->setValue( tskx + 1 );
   }

   if ( ck_highlight->isChecked() )
      plot_subgrid();
   else
      ck_highlight->setChecked( true );      // Triggers plot_subgrid
}

// Show the results of a final fit optimality check
void US_GridView2D::set_optimality( const QList< US_OptimalityPoint >& points,
      double ssq, int iter, const QString& summary )
{
   opt_pts         = points;
   opt_ssq         = ssq;
   opt_iter        = iter;
   pb_optcheck->setEnabled( true );
   te_optinfo ->setText( tr( "Iteration %1:  " ).arg( iter + 1 ) + summary );
   plot_subgrid();
}

// Show a message about the optimality check, clearing previous results
void US_GridView2D::set_optimality_message( const QString& msg )
{
   opt_pts.clear();
   opt_iter        = -1;
   pb_optcheck->setEnabled( true );
   te_optinfo ->setText( msg );
   plot_subgrid();
}

// Show the optimality check progress
void US_GridView2D::set_optimality_progress( int done, int total )
{
   pb_optcheck->setEnabled( done >= total );
   te_optinfo ->setText( tr( "Checking all grid points against the final "
                             "fit residual ...  %1 of %2 subgrids" )
                         .arg( qMax( 0, done - 1 ) ).arg( total - 1 ) );
}

// Plot grid points that could lower the final fit residual, colored by how
// much:  yellow (tiny) to red (at least 1% of the residual sum of squares)
void US_GridView2D::plot_optimality()
{
   if ( opt_pts.isEmpty()  ||  ! ck_optmap->isChecked()  ||  ! show_fit()  ||
        (int)ct_iter->value() - 1 != opt_iter  ||  opt_ssq <= 0.0 )
      return;

   const int    nbins   = 5;
   const double limits[ nbins ] = { 1.0e-9, 1.0e-6, 1.0e-4, 1.0e-3, 1.0e-2 };
   const QColor colors[ nbins ] = { QColor( 255, 255,   0 ),
                                    QColor( 255, 200,   0 ),
                                    QColor( 255, 140,   0 ),
                                    QColor( 255,  70,   0 ),
                                    QColor( 255,   0,   0 ) };
   int    xattr    = x_axis->checkedId();
   int    yattr    = y_axis->checkedId();
   int    ss       = (int)ct_size->value();
   QVector< QVector< double > > xarrs( nbins );
   QVector< QVector< double > > yarrs( nbins );

   for ( int ii = 0; ii < opt_pts.size(); ii++ )
   {
      double rgain    = opt_pts[ ii ].gain / opt_ssq;

      if ( opt_pts[ ii ].insol  ||  rgain < limits[ 0 ] )
         continue;

      int bin         = 0;
      while ( bin < nbins - 1  &&  rgain >= limits[ bin + 1 ] )
         bin++;

      GridPt gpt      = make_point( opt_pts[ ii ].sol );
      xarrs[ bin ] << gpt.vals[ xattr ];
      yarrs[ bin ] << gpt.vals[ yattr ];
      over_sols    << opt_pts[ ii ].sol;
      over_kind    << tr( "Estimated gain %1% of squared residual" )
                      .arg( rgain * 100.0, 0, 'g', 3 );
   }

   bool legend     = true;

   for ( int bb = 0; bb < nbins; bb++ )
   {
      int np          = xarrs[ bb ].size();

      if ( np == 0 )
         continue;

      int sz          = ss + 3 + bb * 2;
      QwtSymbol* symbol = new QwtSymbol( QwtSymbol::Rect,
                                         QBrush( colors[ bb ] ),
                                         QPen( colors[ bb ], 1 ),
                                         QSize( sz, sz ) );
      QwtPlotCurve* curve = point_curve( legend ? tr( "Could Improve Fit" )
                                         : QString( "OPTIMAL_%1" ).arg( bb ),
                                         legend );
      curve->setSymbol ( symbol );
      curve->setSamples( xarrs[ bb ].data(), yarrs[ bb ].data(), np );
      curve->setZ      ( 2.5 );
      over_curves << curve;
      legend          = false;
   }
}
