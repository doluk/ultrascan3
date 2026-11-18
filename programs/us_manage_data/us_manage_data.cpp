//! \file us_manage_data.cpp

#include <QApplication>
#include <QTimer>

#include "us_manage_data.h"
#include "us_data_model.h"
#include "us_data_tree.h"
#include "us_data_process.h"
#include "us_select_item.h"
#include "us_settings.h"
#include "us_gui_settings.h"
#include "us_matrix.h"
#include "us_constants.h"
#include "us_license_t.h"
#include "us_license.h"
#include "us_passwd.h"
#include "us_investigator.h"
#include "us_editor.h"
#include "us_util.h"

const QColor colorRed(   240,   0,   0 );
const QColor colorBlue(    0,   0, 255 );
const QColor colorBrown( 120,  60,   0 );
const QColor colorGreen(   0, 150,   0 );
const QColor colorGray(  110, 110, 110 );
const QColor colorWhite( 255, 255, 240 );

#define timeFmt QString("hh:mm:ss")
#define nowTime() "T="+QDateTime::currentDateTime().toString(timeFmt)

// main program
int main( int argc, char* argv[] )
{
   QApplication application( argc, argv );

   #include "main1.inc"

   // License is OK.  Start up.
   
   US_ManageData w;
   //w.show();                   //!< \memberof QWidget
   if ( w.db != NULL )
      return application.exec();  //!< \memberof QApplication
   else
      return -1;
}

// US_ManageData class constructor
US_ManageData::US_ManageData() : US_Widgets()
{
   dbg_level     = US_Settings::us_debug();
   nmerged       = 0;
   verify_pending = false;

   // set up the GUI
   setWindowTitle( tr( "Manage US DB/Local Data Sets" ) );
   setPalette( US_GuiSettings::frameColor() );
DbgLv(1) << "GUI setup begun";

   // primary layouts
   QHBoxLayout* mainLayout  = new QHBoxLayout( this );
   QVBoxLayout* leftLayout  = new QVBoxLayout();
   QVBoxLayout* rghtLayout  = new QVBoxLayout();
   QGridLayout* dctlLayout  = new QGridLayout();
   QVBoxLayout* smryLayout  = new QVBoxLayout();
   QGridLayout* statLayout  = new QGridLayout();
   QGridLayout* treeLayout  = new QGridLayout();
   mainLayout->setSpacing        ( 2 );
   mainLayout->setContentsMargins( 2, 2, 2, 2 );
   leftLayout->setSpacing        ( 0 );
   leftLayout->setContentsMargins( 0, 1, 0, 1 );
   rghtLayout->setSpacing        ( 0 );
   rghtLayout->setContentsMargins( 0, 1, 0, 1 );
   dctlLayout->setSpacing        ( 1 );
   dctlLayout->setContentsMargins( 0, 0, 0, 0 );
   smryLayout->setSpacing        ( 0 );
   smryLayout->setContentsMargins( 0, 1, 0, 1 );
   statLayout->setSpacing        ( 1 );
   statLayout->setContentsMargins( 0, 0, 0, 0 );
   treeLayout->setSpacing        ( 1 );
   treeLayout->setContentsMargins( 0, 0, 0, 0 );

   // fill in the GUI components
   int row       = 0;

   pb_invtor     = us_pushbutton( tr( "Investigator" ) );
   dctlLayout->addWidget( pb_invtor, row,   0, 1, 3 );
   if ( US_Settings::us_inv_level() < 3 )
      pb_invtor->setEnabled( false );

   personID      = US_Settings::us_inv_ID();
   QString invn  = ( personID > 0 ) ? QString::number( personID ) : "";
   le_invtor     = us_lineedit( invn + ": " + US_Settings::us_inv_name(),
         1, true );
   dctlLayout->addWidget( le_invtor, row++, 3, 1, 5 );

   pb_reset      = us_pushbutton( tr( "Reset" ), false );
   dctlLayout->addWidget( pb_reset,  row,   0, 1, 4 );
   connect( pb_reset,   &QAbstractButton::clicked,
            this,       &US_ManageData::reset );

   pb_scanda     = us_pushbutton( tr( "Scan Data" ) );
   dctlLayout->addWidget( pb_scanda, row++, 4, 1, 4 );
   connect( pb_scanda,  &QAbstractButton::clicked,
            this,       &US_ManageData::scan_data );

   pb_hsedit     = us_pushbutton( tr( "Show All Edits" ) );
   dctlLayout->addWidget( pb_hsedit, row,   0, 1, 4 );
   connect( pb_hsedit,  &QAbstractButton::clicked,
            this,       &US_ManageData::toggle_edits );

   pb_hsmodl     = us_pushbutton( tr( "Show All Models" ) );
   dctlLayout->addWidget( pb_hsmodl, row++, 4, 1, 4 );
   connect( pb_hsmodl,  &QAbstractButton::clicked,
            this,       &US_ManageData::toggle_models );

   pb_hsnois     = us_pushbutton( tr( "Show All Noises" ) );
   dctlLayout->addWidget( pb_hsnois, row,   0, 1, 4 );
   connect( pb_hsnois,  &QAbstractButton::clicked,
            this,       &US_ManageData::toggle_noises );

   pb_helpdt     = us_pushbutton( tr( "Data Tree Help" ) );
   dctlLayout->addWidget( pb_helpdt, row++, 4, 1, 4 );

   pb_help       = us_pushbutton( tr( "Help" ) );
   dctlLayout->addWidget( pb_help,   row,   0, 1, 4 );
   connect( pb_help,    &QAbstractButton::clicked,
            this,       &US_ManageData::help );

   pb_close      = us_pushbutton( tr( "Close" ) );
   dctlLayout->addWidget( pb_close,  row++, 4, 1, 4 );
   connect( pb_close,   &QAbstractButton::clicked,
            this,       &QWidget::close );

   QLabel* lb_runid  = us_label( tr( "RunID:" ) );
   cb_runid      = us_comboBox();
   cb_runid  ->addItem( tr( "ALL" ) );
   cb_runid  ->addItem( tr( "Select individual run" ) );
   dctlLayout->addWidget( lb_runid,  row,   0, 1, 2 );
   dctlLayout->addWidget( cb_runid,  row++, 2, 1, 6 );
   connect( cb_runid,   qOverload< int >( &QComboBox::activated ),
            this,       qOverload< int >( &US_ManageData::selected_runID ) );

   QLabel* lb_triple = us_label( tr( "Triple:" ) );
   cb_triple     = us_comboBox();
   cb_triple ->addItem( "ALL" );
   dctlLayout->addWidget( lb_triple, row,   0, 1, 2 );
   dctlLayout->addWidget( cb_triple, row++, 2, 1, 6 );

   QLabel* lb_source = us_label( tr( "Source:" ) );
   cb_source     = us_comboBox();
   cb_source ->addItem( "ALL" );
   cb_source ->addItem( "DB Only" );
   cb_source ->addItem( "Local Only" );
   cb_source ->addItem( "Exclude Local-Only Trees" );
   cb_source ->addItem( "Exclude DB-Only Trees" );
   dctlLayout->addWidget( lb_source, row,   0, 1, 2 );
   dctlLayout->addWidget( cb_source, row++, 2, 1, 6 );
   connect( cb_source,  &QComboBox::currentTextChanged,
            this,       &US_ManageData::reset );

   pb_invtor->setToolTip( 
      tr( "Use an Investigator dialog to set the database person ID" ) );
   pb_scanda->setToolTip(
      tr( "Scan Database and Local data, with content analysis" ) );
   pb_helpdt->setToolTip(
      tr( "Show a short Help/Legend dialog for notes on the data tree" ) );
   pb_reset ->setToolTip(
      tr( "Reset the data display to its default state" ) );
   pb_help  ->setToolTip(
      tr( "Display detailed US_ManageData documentation text and images" ) );
   pb_close ->setToolTip(
      tr( "Close the US_ManageData window and exit" ) );
   cb_runid ->setToolTip(
      tr( "Select a single run ID with which to populate the data tree " ) );
   cb_triple->setToolTip(
      tr( "Select a single triple of the run to populate the data tree " ) );
   cb_source->setToolTip(
      tr( "Select a filter to limit records based on source (DB/Local) " ) );

   QLabel* lb_info2 = us_banner( tr( "User Data Sets Summary:" ) );
   dctlLayout->addWidget( lb_info2,  row++, 0, 1, 8 );

   QPalette pa( le_invtor->palette() );
   te_status    = us_textedit( );
   te_status->setPalette( US_GuiSettings::normalColor() );
   te_status->setTextBackgroundColor( pa.color( QPalette::Window ) );
   te_status->setTextColor( pa.color( QPalette::WindowText ) );
   te_status->setFont(  QFont( US_Widgets::fixedFont().family(),
                        US_GuiSettings::fontSize() - 2 ) );
   te_status->setText(
      tr( " 0 Combined Total data sets;\n"
          "   0 Combined RawData    records;\n"
          "   0 Combined EditedData records;\n"
          "   0 Combined Model      records;\n"
          "   0 Combined Noise      records.\n"
          " 0 Database Total data sets;\n"
          "   0 Database RawData    records;\n"
          "   0 Database EditedData records;\n"
          "   0 Database Model      records;\n"
          "   0 Database Noise      records.\n"
          " 0 Local    Total data sets;\n"
          "   0 Local    RawData    records;\n"
          "   0 Local    EditedData records;\n"
          "   0 Local    Model      records;\n"
          "   0 Local    Noise      records.\n" ) );
   smryLayout->addWidget( te_status );
   QFontMetrics fm( te_status->font() );
DbgLv(1) << "te_status font family" << te_status->font().family();
   int   fontw  = fm.maxWidth();
         fontw  = ( fontw > 0 ) ? fontw : fm.horizontalAdvance( 'W' );
   int   fonth  = fm.lineSpacing();
   int   minsw  = fontw * 44 + 10;
   int   minsh  = fonth * 18 + 10;
DbgLv(1) << "te_status fw fh  mw mh" << fontw << fonth << " " << minsw << minsh;
   int   maxsw  = ( minsw * 5 ) / 4;
   int   maxsh  = ( minsh * 5 ) / 4;

   if ( maxsw > 500  ||  maxsh > 450 )
   {
      maxsw  = qMin( maxsw, 500 );
      maxsh  = qMin( maxsh, 450 );
      minsw  = ( maxsw * 4 ) / 5;
      minsh  = ( maxsh * 4 ) / 5;
   }

   cb_runid ->setMaximumSize( ( maxsw * 3 ) / 4, fonth * 2 );
   cb_triple->setMaximumSize( ( maxsw * 3 ) / 4, fonth * 2 );
   cb_source->setMaximumSize( ( maxsw * 3 ) / 4, fonth * 2 );

   te_status->setMinimumSize( minsw, minsh );
   te_status->setMaximumSize( maxsw, maxsh );
DbgLv(1) << "te_status minw minh" << minsw << minsh;
DbgLv(1) << "te_status maxw maxh" << maxsw << maxsh;
   te_status->adjustSize();
DbgLv(1) << "te_status size" << te_status->size();

   row  = 0;
   QLabel* lb_progr  = us_label( tr( "Overall:" ) );
   progress          = us_progressBar( 0, 100, 0 );
   statLayout->addWidget( lb_progr,  row,   0, 1, 4 );
   statLayout->addWidget( progress,  row++, 4, 1, 4 );

   // The second bar is the step inside the phase the first one counts, so
   // neither of them ever runs to the end and starts over
   QLabel* lb_sprogr = us_label( tr( "This experiment:" ) );
   pgb_step          = us_progressBar( 0, 100, 0 );
   statLayout->addWidget( lb_sprogr, row,   0, 1, 4 );
   statLayout->addWidget( pgb_step,  row++, 4, 1, 4 );

   lb_status         = us_label( tr( "Status" ) );

   // The status line says what a scan is doing, and what it says includes
   // run identifiers.  Ignored means its text cannot push the window wider
   // than the user sized it, which is what made the window jump about
   // while a scan ran.
   lb_status->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Preferred );
   lb_status->setMinimumWidth( 0 );
   statLayout->addWidget( lb_status, row,   0, 1, 8 );

   // set up data tree; populate with sample data
   tw_recs           = new QTreeWidget();
   tw_recs->setPalette( te_status->palette() );
   treeLayout->addWidget( tw_recs );

   QStringList theads;
   theads << "Type" << "Label" << "SubType" << "Source"
      << "Children\nDB, Local" << "Descendants\nDB, Local";
   ntcols = theads.size();
   tw_recs->setHeaderLabels( theads );
   tw_recs->setFont(  QFont( US_Widgets::fixedFont().family(),
                        US_GuiSettings::fontSize() - 1 ) );
   tw_recs->setObjectName( QString( "tree-widget" ) );
   tw_recs->setAutoFillBackground( true );
   tw_recs->setSelectionMode( QAbstractItemView::ExtendedSelection );

   // Fitting the columns to their contents widens the header, and a wider
   // header asks for a wider window.  The tree takes whatever room the
   // layout gives it instead, and scrolls when the columns need more.
   tw_recs->setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Expanding );
   tw_recs->setSizeAdjustPolicy( QAbstractScrollArea::AdjustIgnored );
   tw_recs->setMinimumWidth( 0 );

   connect( tw_recs, &QTreeWidget::itemPressed,
            this,    &US_ManageData::clickedItem );

   reset_hsbuttons( false, true, true, true );  // hs button labels,tooltips

   // put layouts together for overall layout
   leftLayout->addLayout( dctlLayout );
   leftLayout->addLayout( smryLayout );
   leftLayout->setStretchFactor( smryLayout, 10 );
   leftLayout->addLayout( statLayout );
   rghtLayout->addLayout( treeLayout );

   mainLayout->addLayout( leftLayout );
   mainLayout->addLayout( rghtLayout );
   mainLayout->setStretchFactor( leftLayout, 2 );
   mainLayout->setStretchFactor( rghtLayout, 8 );

   setMinimumWidth( (int)( maxsw * 2.6 ) );

   show();    // display main window before password dialog appears

   // insure we can connect to the database
   US_Passwd pw;
   db            = new US_DB2( pw.getPasswd() );
   if ( db->lastErrno() != US_DB2::OK )
   {
      QMessageBox::information( this,
         tr( "DB Connection Problem" ),
         tr( "There was an error connecting to the database:\n" )
         + db->lastError() + "\n\n"
         + tr( "Program cannot continue without database connection and will be closed." ) );
         //+ tr( "Continuing without database." ) );
      db = NULL;
      return;
   }
DbgLv(1) << "db passwd complete";

   connect( pb_invtor,  &QAbstractButton::clicked,
            this,       &US_ManageData::sel_investigator );
DbgLv(1) << "GUI setup complete";

   // Create an object to handle the data itself
   da_model       = new US_DataModel(                      this );

   // Set needed pointers for class interaction in model object
   da_model->setDatabase( db );
   // The model reports the work inside one experiment, which is the
   // second bar; the first one counts the experiments
   da_model->setProgress( pgb_step,   lb_status );

   // Create an object to handle processing the data (upload,download,remove)
   da_process     = new US_DataProcess( da_model,          this );

   // Create an object to handle the data tree display
   da_tree        = new US_DataTree(    da_model, tw_recs, this );

   // Set needed pointers to sibling classes in model object
   da_model->setSiblings( (QObject*)da_process, (QObject*)da_tree   );

   // An experiment the user opens is read before the ones still queued
   connect( tw_recs, &QTreeWidget::itemExpanded,
            da_tree, &US_DataTree::item_opened );

   // The scan loop is the model's; the window shows what it reports
   connect( da_model, &US_DataModel::scanPhase,
            this,     &US_ManageData::scan_phase );
   connect( da_model, &US_DataModel::scanStep,
            progress, &QProgressBar::setValue );
   connect( da_model, &US_DataModel::runsListed,
            this,     &US_ManageData::runs_listed );
   connect( da_model, &US_DataModel::runMerged,
            this,     &US_ManageData::run_merged );
   connect( da_model, &US_DataModel::orphansAdded, this, [ this ]( int index )
            { da_tree->add_run( index ); reportDataStatus(); } );
   connect( da_model, &US_DataModel::runCompared,
            da_tree,  &US_DataTree::refresh_run );

   // A comparison asked for outside a scan is done in the background, one
   // experiment per turn of the event loop
   connect( da_model, &US_DataModel::verifyRequested,
            this,     [ this ]() { schedule_verifies(); } );
DbgLv(1) << "classes setup complete";

   // Set up initial state of GUI
   connect( pb_helpdt,  &QAbstractButton::clicked,
            da_tree,    &US_DataTree::dtree_help );

   reset();
}

// Reset the GUI
void US_ManageData::reset( void )
{
   if ( da_model->scan_active()  ||  da_model->is_busy() )
      return;                          // never under a running scan

   da_model->dummy_data ();
   da_tree ->build_dtree();

   reportDataStatus();

   int src_flg = cb_source->currentIndex();
   da_model->getRunIDs( runIDs, src_flg );

   cb_runid ->clear();
   cb_runid ->addItem ( tr( "ALL" ) );
   cb_runid ->addItem ( tr( "Select individual run" ) );
   //cb_runid ->addItems( runIDs );
   cb_triple->clear();
   cb_triple->addItem ( tr( "ALL" ) );

}

// toggle edits between hide/show
void US_ManageData::toggle_edits()
{
   bool show = pb_hsedit->text().startsWith( "Show" ); // show or hide?

   if ( show )                                  // experiments hold the raws
      da_tree->toggle_expand( "Experiment", show );

   da_tree->toggle_expand( "Raw",    show );    // expand/collapse one level up

   if ( show )
   {  // for show edits, need only relabel edit button
      reset_hsbuttons( show, true, false, false );
   }

   else
   {  // for hide edits, must toggle labels below; relabel all buttons
      da_tree->toggle_expand( "Model", show );
      da_tree->toggle_expand( "Noise", show );

      reset_hsbuttons( show, true, true, true );
   }
}

// toggle models between hide/show
void US_ManageData::toggle_models()
{
   bool show = pb_hsmodl->text().startsWith( "Show" ); // show or hide?

   da_tree->toggle_expand( "Edited", show );    // expand/collapse one level up

   if ( show )
   { // for show models, must expand levels above; reset edit,model buttons
      da_tree->toggle_expand( "Experiment", show );
      da_tree->toggle_expand( "Raw",    show );

      reset_hsbuttons( show, false, true, true );
   }

   else
   {  // for hide models, must collapse below;  reset model,noise buttons
      da_tree->toggle_expand( "Noise", false );

      reset_hsbuttons( show, false, true, true );
   }
}

// toggle noises between hide/show
void US_ManageData::toggle_noises()
{
   bool show = pb_hsnois->text().startsWith( "Show" )  // show or hide?
            || pb_hsnois->text().startsWith( "Expand" );

   da_tree->toggle_expand( "Model",  show );    // expand/collapse one level up

   if ( show )
   {  // for show noise, must expand above; reset edit,model,noise buttons
      da_tree->toggle_expand( "Experiment", show );
      da_tree->toggle_expand( "Raw",    show );
      da_tree->toggle_expand( "Edited", show );

      reset_hsbuttons( show, true, true, true );
   }

   else
   {  // for hide noise, need only reset noise button
      reset_hsbuttons( show, false, false, true );
   }

}

// change hide/show button text and tooltips for new expand/collapse state
void US_ManageData::reset_hsbuttons( bool show,
      bool edts, bool mods, bool nois )
{
   QString blabl;
   QString btool;

   if ( show )
   {  // show (expand) is the new state
      if ( edts )
      { // Hide Edits 
         pb_hsedit->setText( tr( "Collapse All" ) );
         pb_hsedit->setToolTip( 
            tr( "Collapse All, so Edits and all descendants are hidden" ) );
      }

      if ( mods )
      { // Hide Models
         pb_hsmodl->setText( tr( "Hide All Models" ) );
         pb_hsmodl->setToolTip( 
            tr( "Collapse Edits, so Models and their children are hidden" ) );
      }

      if ( nois )
      { // Hide Noises
         pb_hsnois->setText( tr( "Hide All Noises" ) );
         pb_hsnois->setToolTip( 
            tr( "Collapse Models, so Noises are hidden" ) );
      }
   }

   else
   {  // hide (collapse) is the new state
      if ( edts )
      { // Show Edits 
         pb_hsedit->setText( tr( "Show All Edits" ) );
         pb_hsedit->setToolTip( 
            tr( "Expand Raws, so Edits and parent Raws are shown" ) );
      }

      if ( mods )
      { // Show Models
         pb_hsmodl->setText( tr( "Show All Models" ) );
         pb_hsmodl->setToolTip( 
            tr( "Expand Edits, so Models and their parents are shown" ) );
      }

      if ( nois )
      { // Show Noises
         pb_hsnois->setText( tr( "Expand All" ) );
         pb_hsnois->setToolTip( 
            tr( "Expand All, so Noises and all ancestors are shown" ) );
      }
   }
}
// Start a phase of the scan: what it is doing, and how many steps it has
void US_ManageData::scan_phase( const QString& text, int steps )
{
   progress->setMaximum( qMax( steps, 1 ) );
   progress->setValue  ( 0 );
   pgb_step->setMaximum( 1 );
   pgb_step->setValue  ( 0 );

   status_text( text );
}

// Put a message on the status line, cut to what the line can show
void US_ManageData::status_text( const QString& text )
{
   int width = qMax( lb_status->width() - 4, 80 );

   lb_status->setText( lb_status->fontMetrics().elidedText(
                       text, Qt::ElideMiddle, width ) );
}

// The experiments are listed:  draw the top of the tree
void US_ManageData::runs_listed()
{
   da_tree ->build_dtree();
   fit_tree_columns();
   reportDataStatus();
   nmerged = 0;
}

// One experiment's records are in:  hang them under its row
void US_ManageData::run_merged( int index )
{
   da_tree->fill_run( index );

   if ( ( ++nmerged % 8 ) == 0 )
      reportDataStatus();
}

// Everything a scan could be disturbed by is off while it runs.  The tree
// stays usable for opening experiments -- which only queues them -- but
// offers no actions until the scan is done.
void US_ManageData::set_controls_enabled( bool enabled )
{
   pb_scanda->setEnabled( enabled );
   pb_reset ->setEnabled( enabled );
   pb_hsedit->setEnabled( enabled );
   pb_hsmodl->setEnabled( enabled );
   pb_hsnois->setEnabled( enabled );
   cb_runid ->setEnabled( enabled );
   cb_triple->setEnabled( enabled );
   cb_source->setEnabled( enabled );
   pb_invtor->setEnabled( enabled  &&  US_Settings::us_inv_level() >= 3 );
}

/* Scan the database and the local disk.

   The scan runs in three phases, and the overall bar counts the steps of
   whichever phase is running while the second bar counts the work inside
   one step:

     1. the experiments, which is one query or one pass over the results
        directory, and is enough to draw the top of the tree;
     2. the chain below them, read in bulk -- four queries for the whole
        store rather than four per experiment -- and merged one experiment
        at a time so the tree fills in as it goes;
     3. the checksums, for the experiments that are in both places, since
        those are the only ones that can be out of step with themselves.

   The loop is US_DataModel::scan_data(); this window draws what it reports.
   An experiment the user opens meanwhile moves to the front of whichever
   queue is running.
*/
void US_ManageData::scan_data()
{
   if ( da_model->scan_active()  ||  da_model->is_busy() )   return;

   QString rF = cb_runid ->currentText();
   QString tF = cb_triple->currentText();
   QString sF = cb_source->currentText();
DbgLv(1) << "ScnDM:  Start          " << nowTime();
   da_model->setFilters( rF, tF, sF );  // Set any run,triple,source filters

   set_controls_enabled( false );

   da_model->scan_data();
DbgLv(1) << "ScnDM:  Scan Done      " << nowTime();

   fit_tree_columns();

   pgb_step ->setValue( pgb_step->maximum() );

   reset_hsbuttons( false, true, true, true );  // hs button labels,tooltips

   // reformat and display report on record counts
   reportDataStatus();

   set_controls_enabled( true );

   report_scan_errors();
   report_run_problems();
   review_duplicates();
DbgLv(1) << "ScnDM:  Scan All Done  " << nowTime();
}

// Make sure the comparisons asked for outside a scan get done
void US_ManageData::schedule_verifies( int delay )
{
   if ( verify_pending )   return;     // one pass at a time

   verify_pending = true;
   QTimer::singleShot( delay, this, &US_ManageData::pump_verifies );
}

// Compare the experiments asked for outside a scan, one per event-loop turn
//
// Opening an experiment only asks for it to be compared, so opening rows --
// or "Show All Edits", which opens all of them -- never waits for a
// comparison; this does them in the background, one at a time.
void US_ManageData::pump_verifies()
{
   verify_pending = false;

   if ( da_model->scan_active() )   return;   // the scan does its own queue

   if ( da_model->is_busy() )
   {  // something else is reading just now: try again shortly
      schedule_verifies( 50 );
      return;
   }

   int index = da_model->next_pending_verify();

   if ( index < 0 )   return;

   status_text( tr( "Comparing %1 ..." )
                .arg( da_model->run_entry( index ).runID ) );

   if ( da_model->verify_run( index ) )
      da_tree->refresh_run( index );

   if ( da_model->pending_verifies() > 0 )
      schedule_verifies();
   else
      status_text( tr( "Comparison complete" ) );
}

// Say what went wrong in a scan, rather than leaving it in the debug log
void US_ManageData::report_scan_errors()
{
   QStringList errors = da_model->scan_errors();

   if ( da_model->db_listing_failed() )
   {
      QMessageBox::warning( this, tr( "Database Not Listed" ),
         tr( "The database could not be listed, so this tree shows only "
             "what is on local disk:  a record shown as \"Local\" may be in "
             "the database too.\n\n" ) + errors.join( "\n" ) );
      return;
   }

   if ( errors.isEmpty() )   return;

   int     nshow = qMin( errors.size(), 12 );
   QString text  = QStringList( errors.mid( 0, nshow ) ).join( "\n" );

   if ( nshow < errors.size() )
      text += tr( "\n... and %1 more" ).arg( errors.size() - nshow );

   QMessageBox::warning( this, tr( "Scan Errors" ),
      tr( "The scan ran into %1 error(s); the records concerned may be "
          "missing, or read as not yet compared:\n\n" )
      .arg( errors.size() ) + text );
}

// Say which runs should not be acted on as they are, and why
void US_ManageData::report_run_problems()
{
   QStringList problems = da_model->run_problems();

   if ( problems.isEmpty() )   return;

   int     nshow = qMin( problems.size(), 12 );
   QString text  = QStringList( problems.mid( 0, nshow ) ).join( "\n" );

   if ( nshow < problems.size() )
      text += tr( "\n... and %1 more" ).arg( problems.size() - nshow );

   QMessageBox::information( this, tr( "Runs With Problems" ),
      tr( "%1 run(s) are marked \"(!)\" in the tree; hold the mouse over "
          "one to see why:\n\n" ).arg( problems.size() ) + text +
      tr( "\n\nRun IDs with dots in them are from older versions of "
          "UltraScan and are still shown; new runs may use only letters, "
          "digits, '_' and '-'." ) );
}

/* Tell the user about records that share a GUID.

   This used to be asked once per experiment and per source, in the middle
   of the scan; it is asked once, over the whole tree, when the scan is done
   -- so records duplicated across experiments are found too.
*/
void US_ManageData::review_duplicates()
{
   QList< US_DataModel::Duplicate > dups = da_model->find_duplicates();

   if ( dups.isEmpty() )   return;

   const char* rtyps[] = { "Experiment", "RawData", "EditedData",
                           "Model", "Noise" };
   int nrecs = 0;

   for ( int ii = 0; ii < dups.size(); ii++ )
      nrecs += dups[ ii ].rows.size();

   QMessageBox msgBox( this );
   msgBox.setWindowTitle( tr( "Duplicate Records" ) );
   msgBox.setText(
      tr( "There are %1 records that have the same GUID as another.\n" )
         .arg( nrecs ) +
      tr( "You should correct the situation before proceeding.\n" ) +
      tr( "  Click \"Ok\" to see details.\n" ) +
      tr( "  Click \"Ignore\" to go on.\n" ) );
   msgBox.setStandardButtons( QMessageBox::Ok | QMessageBox::Ignore );
   msgBox.setDefaultButton( QMessageBox::Ok );

   if ( msgBox.exec() != QMessageBox::Ok )   return;

   QString msg =
      tr( "Review the details below on duplicate records.\n" ) +
      tr( "Save or Print the contents of this message.\n" ) +
      tr( "Decide which of the duplicates should be removed.\n\n" );

   for ( int ii = 0; ii < dups.size(); ii++ )
   {
      const US_DataModel::Duplicate& dup = dups[ ii ];

      msg += tr( "%1 GUID:  " ).arg( rtyps[ dup.recType ] ) + dup.guid + "\n";

      for ( int jj = 0; jj < dup.rows.size(); jj++ )
      {
         US_DataModel::DataDesc desc = da_model->row_datadesc( dup.rows[ jj ] );

         msg += tr( "  Experiment     :  " )
                   + da_model->run_id_of_row( dup.rows[ jj ] ) + "\n" +
                tr( "  Label          :  " ) + desc.label + "\n" +
                tr( "  Description    :  " ) + desc.description + "\n" +
                tr( "  ParentGUID     :  " ) + desc.parentGUID + "\n" +
                tr( "  DB record ID   :  %1" ).arg( desc.recordID ) + "\n" +
                tr( "  File           :  " ) + desc.filename + "\n" +
                tr( "  File Last Mod  :  " ) + desc.filemodDate + "\n" +
                tr( "  DB Last Mod    :  " ) + desc.lastmodDate + "\n";
      }

      msg += "\n";
   }

   QString fileexts = tr( "Text,Log files (*.txt *.log);;" )
      + tr( "All files (*)" );
   US_Editor* editd = new US_Editor( US_Editor::LOAD, true, fileexts );
   editd->setAttribute( Qt::WA_DeleteOnClose );
   editd->setWindowTitle( tr( "Data Set Duplicate GUID Details" ) );
   editd->move( QCursor::pos() + QPoint( 200, 200 ) );
   editd->resize( 600, 500 );
   editd->e->setFont( QFont( US_Widgets::fixedFont().family(),
                      US_GuiSettings::fontSize() ) );
   editd->e->setText( msg );
   editd->show();
}

/* Fit the tree columns to what is in them, without letting that decide how
   wide the window is: a column is never given more than a third of the
   tree, and the rest scrolls.
*/
void US_ManageData::fit_tree_columns()
{
   int limit = qMax( tw_recs->viewport()->width() / 3, 120 );

   for ( int jj = 0; jj < ntcols; jj++ )
   {
      tw_recs->resizeColumnToContents( jj );

      if ( tw_recs->columnWidth( jj ) > limit )
         tw_recs->setColumnWidth( jj, limit );
   }
}

// Open dialog and get investigator when button clicked
void US_ManageData::sel_investigator()
{
   US_Investigator* inv_dialog = new US_Investigator( true, personID );

   connect( inv_dialog,
      &US_Investigator::investigator_accepted,
      this, &US_ManageData::assign_investigator );

   inv_dialog->exec();
}

// Assign an investigator string in proper id:lastname,firstname form
void US_ManageData::assign_investigator( int invID )
{
   personID   = invID;
   le_invtor->setText( QString::number( personID )
         + ": " + US_Settings::us_inv_name() );

   da_model->setDatabase( db );
}

// Handle a mouse press on a row cell
void US_ManageData::clickedItem( QTreeWidgetItem* item )
{
   // Only a right-button press does anything: it brings up the context
   // menu.  Selecting a row reads and compares nothing -- opening an
   // experiment does that (US_DataTree::item_opened).
   if ( QApplication::mouseButtons() == Qt::RightButton )
      da_tree->row_context_menu( item );
}

// report status of data
void US_ManageData::reportDataStatus()
{
   US_DataModel::DataDesc cdesc;

   // count each type of record
   ncrecs     = da_model->recCount();
   ndrecs     = da_model->recCountDB();
   nlrecs     = da_model->recCountLoc();
   ncraws     = ncedts = ncmods = ncnois = 0;
   ndraws     = ndedts = ndmods = ndnois = 0;
   nlraws     = nledts = nlmods = nlnois = 0;

   for ( int ii = 0; ii < ncrecs; ii++ )
   {
      cdesc   = da_model->row_datadesc( ii );

      if ( cdesc.recType == 1 )
      {
         ncraws++;
         ndraws    += ( cdesc.recState & US_DataModel::REC_DB ) != 0 ? 1 : 0;
         nlraws    += ( cdesc.recState & US_DataModel::REC_LO ) != 0 ? 1 : 0;
      }

      else if ( cdesc.recType == 2 )
      {
         ncedts++;
         ndedts    += ( cdesc.recState & US_DataModel::REC_DB ) != 0 ? 1 : 0;
         nledts    += ( cdesc.recState & US_DataModel::REC_LO ) != 0 ? 1 : 0;
      }

      else if ( cdesc.recType == 3 )
      {
         ncmods++;
         ndmods    += ( cdesc.recState & US_DataModel::REC_DB ) != 0 ? 1 : 0;
         nlmods    += ( cdesc.recState & US_DataModel::REC_LO ) != 0 ? 1 : 0;
      }

      else if ( cdesc.recType == 4 )
      {
         ncnois++;
         ndnois    += ( cdesc.recState & US_DataModel::REC_DB ) != 0 ? 1 : 0;
         nlnois    += ( cdesc.recState & US_DataModel::REC_LO ) != 0 ? 1 : 0;
      }
   }

   // In case there was filtering, recompute counts
   ndrecs     = ndraws + ndedts + ndmods + ndnois;
   nlrecs     = nlraws + nledts + nlmods + nlnois;
   ncrecs     = ndrecs + nlrecs;
   ncraws     = ndraws + nlraws;
   ncedts     = ndedts + nledts;
   ncmods     = ndmods + nlmods;
   ncnois     = ndnois + nlnois;

   // Reformat and display report on record counts
   te_status->setText(
      QString::asprintf( "%5d", da_model->runCount() ) +
      tr( " Experiments;\n"                    ) +
      QString::asprintf( "%5d", ncrecs ) +
      tr( " Combined Total data sets;\n  "    ) +
      QString::asprintf( "%5d", ncraws ) +
      tr( " Combined RawData    records;\n  " ) +
      QString::asprintf( "%5d", ncedts ) +
      tr( " Combined EditedData records;\n  " ) +
      QString::asprintf( "%5d", ncmods ) +
      tr( " Combined Model      records;\n  " ) +
      QString::asprintf( "%5d", ncnois ) +
      tr( " Combined Noise      records.\n"   ) +
      QString::asprintf( "%5d", ndrecs ) +
      tr( " Database Total data sets;\n  "    ) +
      QString::asprintf( "%5d", ndraws ) +
      tr( " Database RawData    records;\n  " ) +
      QString::asprintf( "%5d", ndedts ) +
      tr( " Database EditedData records;\n  " ) +
      QString::asprintf( "%5d", ndmods ) +
      tr( " Database Model      records;\n  " ) +
      QString::asprintf( "%5d", ndnois ) +
      tr( " Database Noise      records.\n"   ) +
      QString::asprintf( "%5d", nlrecs ) +
      tr( " Local    Total data sets;\n  "    ) +
      QString::asprintf( "%5d", nlraws ) +
      tr( " Local    RawData    records;\n  " ) +
      QString::asprintf( "%5d", nledts ) +
      tr( " Local    EditedData records;\n  " ) +
      QString::asprintf( "%5d", nlmods ) +
      tr( " Local    Model      records;\n  " ) +
      QString::asprintf( "%5d", nlnois ) +
      tr( " Local    Noise      records.\n"   ) );
}

// Slot to handle a newly select runID filter
void US_ManageData::selected_runID( QString selrunID )
{
DbgLv(0) << "selected_runID: selrunID" << selrunID;
   if ( selrunID.isEmpty()  ||  selrunID == tr("ALL") )
   {
      cb_triple->clear();
      cb_triple->addItem( tr( "ALL" ) );
      return;
   }

   if ( selrunID.startsWith( tr( "Select individual" ) ) )
   {  // Combo box select is "Select individual run" so present list dialog
      int selx     = -1;
      QStringList hdrs;
      QString dtitl = tr( "Select Run ID" );
      hdrs << "Run ID";
      runid_data.clear();

      for ( int ii = 0; ii < runIDs.count(); ii++ )
      {  // Build data consisting of a single Run ID column
         runid_data << QStringList( runIDs[ ii ] );
      }

      US_SelectItem srdiag( runid_data, hdrs, dtitl, &selx, 1 );

      if ( srdiag.exec() != QDialog::Accepted  ||  selx < 0 )
         return;                       // no run picked: no triples to list

      {  // Add selected run to comboBox and set Run ID variable
         QString srunID = runid_data[ selx ][ 0 ];
         selrunID       = srunID;      // the triples are this run's
         cb_runid->disconnect();
         cb_runid->clear();
         cb_runid->addItem( tr( "ALL" ) );
         cb_runid->addItem( tr( "Select individual run" ) );
         cb_runid->addItem( srunID );
         cb_runid->setCurrentIndex( 2 );
         connect( cb_runid,   qOverload< int >( &QComboBox::activated ),
                  this,       qOverload< int >( &US_ManageData::selected_runID ) );
      }
   }

   lb_status->setText( tr( "Scanning triples for a run..." ) );
   qApp->processEvents();
   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );
   da_model->getTriples( triples, selrunID );
DbgLv(0) << "selected_runID: da_m getTriples size" << triples.size();

   cb_triple->clear();
   cb_triple->addItem( "ALL" );
   cb_triple->addItems( triples );
   QApplication::restoreOverrideCursor();
   int ntriple = triples.size();

   lb_status->setText(
         ( ntriple == 1 ) ? tr( "The run has a single triple" )
         : tr( "%1 triples are available to select" ).arg( ntriple ) );

}

// Slot to handle a newly select runID filter
void US_ManageData::selected_runID( int index )
{
   selected_runID( cb_runid->itemText( index ) );
}

