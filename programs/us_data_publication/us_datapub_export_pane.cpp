//! \file us_datapub_export_pane.cpp
#include "us_datapub_export_pane.h"
#include "us_datapub_manifest.h"

#include "us_gui_settings.h"
#include "us_settings.h"
#include "us_passwd.h"
#include "us_project_gui.h"
#include "us_select_runs.h"
#include "us_model_loader.h"
#include "us_noise_loader.h"
#include "us_editor.h"

// The roles the tree items carry
#define ROLE_GUID   ( Qt::UserRole     )
#define ROLE_KIND   ( Qt::UserRole + 1 )
#define KIND_RUN    "run"
#define KIND_RAW    "raw"
#define KIND_EDIT   "edit"
#define KIND_MODEL  "model"

US_DataPubExportPane::US_DataPubExportPane( QWidget* parent )
   : US_Widgets( true, parent )
{
   catalog_open = false;
   updating     = false;

   setPalette( US_GuiSettings::frameColor() );

   QVBoxLayout* main = new QVBoxLayout( this );
   main->setContentsMargins( 2, 2, 2, 2 );
   main->setSpacing        ( 2 );

   // ---- the data source --------------------------------------------------
   dkdb_cntrls = new US_Disk_DB_Controls(
                 US_Settings::default_data_location() );
   connect( dkdb_cntrls, &US_Disk_DB_Controls::changed,
            this,        &US_DataPubExportPane::source_changed );
   main->addWidget( us_banner( tr( "Export data from" ) ) );
   main->addLayout( dkdb_cntrls );

   // ---- project and runs -------------------------------------------------
   QGridLayout* pick = new QGridLayout;
   int row = 0;

   pb_project   = us_pushbutton( tr( "Select Project" ) );
   le_project   = us_lineedit( tr( "(no project selected)" ), 0, true );
   pb_clearproj = us_pushbutton( tr( "Clear" ) );
   pick->addWidget( pb_project,   row, 0, 1, 1 );
   pick->addWidget( le_project,   row, 1, 1, 3 );
   pick->addWidget( pb_clearproj, row++, 4, 1, 1 );

   pb_runs      = us_pushbutton( tr( "Select Experiment(s)" ) );
   le_runs      = us_lineedit( tr( "(no run selected)" ), 0, true );
   pb_clearruns = us_pushbutton( tr( "Clear" ) );
   pick->addWidget( pb_runs,      row, 0, 1, 1 );
   pick->addWidget( le_runs,      row, 1, 1, 3 );
   pick->addWidget( pb_clearruns, row++, 4, 1, 1 );

   main->addLayout( pick );

   connect( pb_project,   &QPushButton::clicked,
            this,         &US_DataPubExportPane::select_project );
   connect( pb_clearproj, &QPushButton::clicked,
            this,         &US_DataPubExportPane::clear_project );
   connect( pb_runs,      &QPushButton::clicked,
            this,         &US_DataPubExportPane::select_runs );
   connect( pb_clearruns, &QPushButton::clicked,
            this,         &US_DataPubExportPane::clear_runs );

   // ---- the two trees ----------------------------------------------------
   QHBoxLayout* trees = new QHBoxLayout;

   QVBoxLayout* left  = new QVBoxLayout;
   left->addWidget( us_banner( tr( "Experiments, raw data and edits" ) ) );

   tw_data = new QTreeWidget( this );
   tw_data->setFrameStyle( QFrame::NoFrame );
   tw_data->setPalette   ( US_GuiSettings::editColor() );
   tw_data->setFont      ( QFont( US_GuiSettings::fontFamily(),
                                  US_GuiSettings::fontSize() ) );
   tw_data->setColumnCount( 2 );
   QStringList dheaders;
   dheaders << tr( "Run / Raw Data / Edit" ) << tr( "GUID" );
   tw_data->setHeaderLabels( dheaders );
   left->addWidget( tw_data );

   QHBoxLayout* dbtns = new QHBoxLayout;
   pb_allraw   = us_pushbutton( tr( "All Raw" ) );
   pb_noraw    = us_pushbutton( tr( "No Raw" ) );
   pb_lastedit = us_pushbutton( tr( "Latest Edit" ) );
   pb_alledit  = us_pushbutton( tr( "All Edits" ) );
   pb_noedit   = us_pushbutton( tr( "No Edits" ) );
   dbtns->addWidget( pb_allraw );
   dbtns->addWidget( pb_noraw );
   dbtns->addWidget( pb_lastedit );
   dbtns->addWidget( pb_alledit );
   dbtns->addWidget( pb_noedit );
   left->addLayout( dbtns );

   QVBoxLayout* right = new QVBoxLayout;
   right->addWidget( us_banner( tr( "Selected models and noise" ) ) );

   tw_models = new QTreeWidget( this );
   tw_models->setFrameStyle( QFrame::NoFrame );
   tw_models->setPalette   ( US_GuiSettings::editColor() );
   tw_models->setFont      ( QFont( US_GuiSettings::fontFamily(),
                                    US_GuiSettings::fontSize() ) );
   tw_models->setColumnCount( 2 );
   QStringList mheaders;
   mheaders << tr( "Run / Raw Data / Edit / Model" ) << tr( "GUID" );
   tw_models->setHeaderLabels( mheaders );
   right->addWidget( tw_models );

   QHBoxLayout* mbtns = new QHBoxLayout;
   pb_models      = us_pushbutton( tr( "Select Models..." ) );
   pb_noises      = us_pushbutton( tr( "Select Noise..." ) );
   pb_clearmodels = us_pushbutton( tr( "Clear Models" ) );
   mbtns->addWidget( pb_models );
   mbtns->addWidget( pb_noises );
   mbtns->addWidget( pb_clearmodels );
   right->addLayout( mbtns );

   trees->addLayout( left  );
   trees->addLayout( right );
   main->addLayout( trees );

   connect( pb_allraw,   &QPushButton::clicked,
            this,        &US_DataPubExportPane::all_raw );
   connect( pb_noraw,    &QPushButton::clicked,
            this,        &US_DataPubExportPane::no_raw );
   connect( pb_lastedit, &QPushButton::clicked,
            this,        &US_DataPubExportPane::latest_edits );
   connect( pb_alledit,  &QPushButton::clicked,
            this,        &US_DataPubExportPane::all_edits );
   connect( pb_noedit,   &QPushButton::clicked,
            this,        &US_DataPubExportPane::no_edits );
   connect( pb_models,   &QPushButton::clicked,
            this,        &US_DataPubExportPane::select_models );
   connect( pb_noises,   &QPushButton::clicked,
            this,        &US_DataPubExportPane::select_noises );
   connect( pb_clearmodels, &QPushButton::clicked,
            this,        &US_DataPubExportPane::clear_models );
   connect( tw_data,     &QTreeWidget::itemChanged,
            this,        &US_DataPubExportPane::item_changed );

   // ---- scope, bundle and summary ----------------------------------------
   QGridLayout* opts = new QGridLayout;
   row = 0;

   opts->addWidget( us_label( tr( "Export up to:" ) ), row, 0, 1, 1 );
   cb_scope = us_comboBox();
   cb_scope->addItems( US_DataPub::scopeKeys() );
   cb_scope->setCurrentIndex( cb_scope->count() - 1 );
   opts->addWidget( cb_scope, row, 1, 1, 1 );

   QGridLayout* tmstlay = us_checkbox( tr( "Include time state" ), ck_tmst,
                                       true );
   opts->addLayout( tmstlay, row, 2, 1, 1 );

   opts->addWidget( us_label( tr( "Comment:" ) ), row, 3, 1, 1 );
   le_comment = us_lineedit( "", 0, false );
   opts->addWidget( le_comment, row++, 4, 1, 1 );

   opts->addWidget( us_label( tr( "Bundle file:" ) ), row, 0, 1, 1 );
   le_bundle = us_lineedit( "", 0, false );
   pb_browse = us_pushbutton( tr( "Browse..." ) );
   opts->addWidget( le_bundle, row, 1, 1, 3 );
   opts->addWidget( pb_browse, row++, 4, 1, 1 );

   opts->addWidget( us_label( tr( "Summary:" ) ), row, 0, 1, 1 );
   le_summary = us_lineedit( tr( "Nothing selected" ), 0, true );
   pb_details = us_pushbutton( tr( "Details..." ) );
   opts->addWidget( le_summary, row, 1, 1, 3 );
   opts->addWidget( pb_details, row++, 4, 1, 1 );

   main->addLayout( opts );

   connect( pb_browse,  &QPushButton::clicked,
            this,       &US_DataPubExportPane::browse_bundle );
   connect( pb_details, &QPushButton::clicked,
            this,       &US_DataPubExportPane::show_details );

   // ---- progress and buttons ---------------------------------------------
   pgb_progress = us_progressBar( 0, 100, 0 );
   main->addWidget( pgb_progress );

   te_status = us_textedit();
   te_status->setReadOnly( true );
   te_status->setMaximumHeight( 90 );
   main->addWidget( te_status );

   QHBoxLayout* buttons = new QHBoxLayout;
   QPushButton* pb_help = us_pushbutton( tr( "Help" ) );
   pb_reset  = us_pushbutton( tr( "Reset" ) );
   pb_export = us_pushbutton( tr( "Export Bundle" ) );
   buttons->addWidget( pb_help );
   buttons->addWidget( pb_reset );
   buttons->addWidget( pb_export );
   main->addLayout( buttons );

   connect( pb_help,   &QPushButton::clicked,
            this,      &US_DataPubExportPane::help );
   connect( pb_reset,  &QPushButton::clicked,
            this,      &US_DataPubExportPane::reset );
   connect( pb_export, &QPushButton::clicked,
            this,      &US_DataPubExportPane::run_export );

   updateSummary();
}

QString US_DataPubExportPane::password( void )
{
   US_Passwd pw;

   return pw.getPasswd();
}

bool US_DataPubExportPane::openCatalog( void )
{
   if ( catalog_open )  return true;

   QString error;
   bool    useDb = dkdb_cntrls->db();

   if ( ! catalog.open( useDb, useDb ? password() : QString(), error ) )
   {
      QMessageBox::warning( this, tr( "Data Source Problem" ), error );
      return false;
   }

   catalog_open = true;

   return true;
}

// --------------------------------------------------------------- the source

void US_DataPubExportPane::source_changed( bool )
{
   catalog_open = false;
   reset();
}

// -------------------------------------------------------------- the project

void US_DataPubExportPane::select_project( void )
{
   if ( ! openCatalog() )  return;

   int state = dkdb_cntrls->db() ? US_Disk_DB_Controls::DB
                                 : US_Disk_DB_Controls::Disk;
   US_ProjectGui* dialog = new US_ProjectGui( true, state );

   connect( dialog, &US_ProjectGui::updateProjectGuiSelection,
            this,   &US_DataPubExportPane::project_chosen );

   dialog->exec();
   qApp->processEvents();

   delete dialog;
}

void US_DataPubExportPane::project_chosen( US_Project& project )
{
   project_guid = project.projectGUID;
   project_id   = QString::number( project.projectID );
   project_desc = project.projectDesc;

   le_project->setText( QString( "%1  [%2]" ).arg( project_desc )
                        .arg( project_guid ) );

   // Runs that no longer belong to this project are dropped
   QList< US_DataPubCatalog::Run > kept;

   for ( int ii = 0; ii < runs.size(); ii++ )
      if ( runs[ ii ].projectGUID == project_guid )
         kept << runs[ ii ];

   if ( kept.size() != runs.size() )
   {
      runs = kept;
      buildDataTree();
   }

   updateSummary();
}

void US_DataPubExportPane::clear_project( void )
{
   project_guid.clear();
   project_id.clear();
   project_desc.clear();
   le_project->setText( tr( "(no project selected)" ) );
   updateSummary();
}

// ----------------------------------------------------------------- the runs

void US_DataPubExportPane::select_runs( void )
{
   if ( ! openCatalog() )  return;

   QString     error;
   QStringList allowed;

   if ( ! project_guid.isEmpty() )
   {
      QList< US_DataPubCatalog::Run > found = catalog.runs( project_guid,
                                                            error );

      for ( int ii = 0; ii < found.size(); ii++ )
         allowed << found[ ii ].runID;

      if ( allowed.isEmpty() )
      {
         QMessageBox::information( this, tr( "No Runs" ),
            tr( "No runs of project \"%1\" were found in the %2." )
            .arg( project_desc )
            .arg( dkdb_cntrls->db() ? tr( "database" )
                                    : tr( "local results directory" ) ) );
         return;
      }
   }

   QStringList     runIDs;
   US_SelectRuns*  dialog = allowed.isEmpty()
                            ? new US_SelectRuns( dkdb_cntrls->db(), runIDs )
                            : new US_SelectRuns( dkdb_cntrls->db(), runIDs,
                                                 allowed );
   dialog->exec();
   qApp->processEvents();
   delete dialog;

   if ( runIDs.isEmpty() )  return;

   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );
   runs.clear();

   for ( int ii = 0; ii < runIDs.size(); ii++ )
   {
      US_DataPubCatalog::Run run;

      if ( ! catalog.runByID( runIDs[ ii ], run, error ) )
      {
         te_status->append( error );
         continue;
      }

      if ( ! catalog.loadRunDetails( run, error ) )
      {
         te_status->append( error );
         continue;
      }

      runs << run;
   }

   QApplication::restoreOverrideCursor();

   // Adopt the project of the first run when none was picked by hand
   if ( project_guid.isEmpty()  &&  ! runs.isEmpty() )
   {
      project_guid = runs[ 0 ].projectGUID;
      project_id   = runs[ 0 ].projectID;
      project_desc = runs[ 0 ].projectDesc;

      if ( ! project_guid.isEmpty() )
         le_project->setText( tr( "%1  [from run %2]" ).arg( project_desc )
                              .arg( runs[ 0 ].runID ) );
   }

   models.clear();
   noises.clear();
   sel_models.clear();
   sel_noises.clear();

   buildDataTree();
   latest_edits();
   buildModelTree();
   updateSummary();
}

void US_DataPubExportPane::clear_runs( void )
{
   runs.clear();
   models.clear();
   noises.clear();
   sel_models.clear();
   sel_noises.clear();
   buildDataTree();
   buildModelTree();
   updateSummary();
}

// ---------------------------------------------------------------- the trees

void US_DataPubExportPane::buildDataTree( void )
{
   updating = true;
   tw_data->clear();

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      const US_DataPubCatalog::Run& run = runs[ ii ];
      QStringList rtext;
      rtext << run.runID << run.guid;

      QTreeWidgetItem* ritem = new QTreeWidgetItem( rtext );
      ritem->setData( 0, ROLE_GUID, run.guid );
      ritem->setData( 0, ROLE_KIND, QString( KIND_RUN ) );
      tw_data->addTopLevelItem( ritem );

      for ( int jj = 0; jj < run.raws.size(); jj++ )
      {
         const US_DataPubCatalog::Raw& raw = run.raws[ jj ];
         QStringList wtext;
         wtext << raw.filename << raw.guid;

         QTreeWidgetItem* witem = new QTreeWidgetItem( wtext );
         witem->setData( 0, ROLE_GUID, raw.guid );
         witem->setData( 0, ROLE_KIND, QString( KIND_RAW ) );
         witem->setFlags( witem->flags() | Qt::ItemIsUserCheckable );
         witem->setCheckState( 0, Qt::Checked );
         ritem->addChild( witem );

         for ( int kk = 0; kk < raw.edits.size(); kk++ )
         {
            const US_DataPubCatalog::Edit& edit = raw.edits[ kk ];
            QStringList etext;
            etext << edit.filename << edit.guid;

            QTreeWidgetItem* eitem = new QTreeWidgetItem( etext );
            eitem->setData( 0, ROLE_GUID, edit.guid );
            eitem->setData( 0, ROLE_KIND, QString( KIND_EDIT ) );
            eitem->setFlags( eitem->flags() | Qt::ItemIsUserCheckable );
            eitem->setCheckState( 0, Qt::Unchecked );
            witem->addChild( eitem );
         }
      }

      ritem->setExpanded( true );
   }

   tw_data->resizeColumnToContents( 0 );
   updating = false;

   le_runs->setText( runs.isEmpty() ? tr( "(no run selected)" )
                     : tr( "%1 run(s) selected" ).arg( runs.size() ) );
}

void US_DataPubExportPane::buildModelTree( void )
{
   tw_models->clear();

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      const US_DataPubCatalog::Run& run = runs[ ii ];
      QTreeWidgetItem* ritem = nullptr;

      for ( int jj = 0; jj < run.raws.size(); jj++ )
      {
         const US_DataPubCatalog::Raw& raw = run.raws[ jj ];
         QTreeWidgetItem* witem = nullptr;

         for ( int kk = 0; kk < raw.edits.size(); kk++ )
         {
            const US_DataPubCatalog::Edit& edit = raw.edits[ kk ];
            QTreeWidgetItem* eitem = nullptr;

            for ( int mm = 0; mm < models.size(); mm++ )
            {
               const US_DataPubCatalog::Model& model = models[ mm ];

               if ( model.editGUID != edit.guid )               continue;
               if ( ! sel_models.contains( model.guid ) )       continue;

               if ( ritem == nullptr )
               {
                  QStringList rtext;
                  rtext << run.runID << run.guid;
                  ritem = new QTreeWidgetItem( rtext );
                  tw_models->addTopLevelItem( ritem );
                  ritem->setExpanded( true );
               }

               if ( witem == nullptr )
               {
                  QStringList wtext;
                  wtext << raw.filename << raw.guid;
                  witem = new QTreeWidgetItem( wtext );
                  ritem->addChild( witem );
                  witem->setExpanded( true );
               }

               if ( eitem == nullptr )
               {
                  QStringList etext;
                  etext << edit.filename << edit.guid;
                  eitem = new QTreeWidgetItem( etext );
                  witem->addChild( eitem );
                  eitem->setExpanded( true );
               }

               QStringList mtext;
               mtext << model.description << model.guid;
               QTreeWidgetItem* mitem = new QTreeWidgetItem( mtext );
               mitem->setData( 0, ROLE_GUID, model.guid );
               mitem->setData( 0, ROLE_KIND, QString( KIND_MODEL ) );
               eitem->addChild( mitem );

               for ( int nn = 0; nn < noises.size(); nn++ )
               {
                  if ( noises[ nn ].modelGUID != model.guid )        continue;
                  if ( ! sel_noises.contains( noises[ nn ].guid ) )  continue;

                  QStringList ntext;
                  ntext << tr( "%1 noise" ).arg( noises[ nn ].noiseType )
                        << noises[ nn ].guid;
                  mitem->addChild( new QTreeWidgetItem( ntext ) );
               }

               mitem->setExpanded( true );
            }
         }
      }
   }

   tw_models->resizeColumnToContents( 0 );
}

QStringList US_DataPubExportPane::checkedRaws( void ) const
{
   QStringList guids;

   for ( int ii = 0; ii < tw_data->topLevelItemCount(); ii++ )
   {
      QTreeWidgetItem* ritem = tw_data->topLevelItem( ii );

      for ( int jj = 0; jj < ritem->childCount(); jj++ )
      {
         QTreeWidgetItem* witem = ritem->child( jj );

         if ( witem->checkState( 0 ) != Qt::Checked )  continue;

         guids << witem->data( 0, ROLE_GUID ).toString();
      }
   }

   return guids;
}

QStringList US_DataPubExportPane::checkedEdits( void ) const
{
   QStringList guids;

   for ( int ii = 0; ii < tw_data->topLevelItemCount(); ii++ )
   {
      QTreeWidgetItem* ritem = tw_data->topLevelItem( ii );

      for ( int jj = 0; jj < ritem->childCount(); jj++ )
      {
         QTreeWidgetItem* witem = ritem->child( jj );

         if ( witem->checkState( 0 ) != Qt::Checked )  continue;

         for ( int kk = 0; kk < witem->childCount(); kk++ )
         {
            QTreeWidgetItem* eitem = witem->child( kk );

            if ( eitem->checkState( 0 ) != Qt::Checked )  continue;

            guids << eitem->data( 0, ROLE_GUID ).toString();
         }
      }
   }

   return guids;
}

QTreeWidgetItem* US_DataPubExportPane::itemFor( QTreeWidget* tree,
                                                const QString& guid ) const
{
   QTreeWidgetItemIterator it( tree );

   while ( *it )
   {
      if ( (*it)->data( 0, ROLE_GUID ).toString() == guid )  return *it;

      ++it;
   }

   return nullptr;
}

// -------------------------------------------------------- the check buttons

void US_DataPubExportPane::all_raw( void )
{
   updating = true;

   for ( int ii = 0; ii < tw_data->topLevelItemCount(); ii++ )
   {
      QTreeWidgetItem* ritem = tw_data->topLevelItem( ii );

      for ( int jj = 0; jj < ritem->childCount(); jj++ )
         ritem->child( jj )->setCheckState( 0, Qt::Checked );
   }

   updating = false;
   updateSummary();
}

void US_DataPubExportPane::no_raw( void )
{
   QStringList needed;

   for ( int ii = 0; ii < tw_data->topLevelItemCount(); ii++ )
   {
      QTreeWidgetItem* ritem = tw_data->topLevelItem( ii );

      for ( int jj = 0; jj < ritem->childCount(); jj++ )
      {
         QTreeWidgetItem* witem = ritem->child( jj );

         for ( int kk = 0; kk < witem->childCount(); kk++ )
         {
            QTreeWidgetItem* eitem = witem->child( kk );

            if ( eitem->checkState( 0 ) != Qt::Checked )  continue;

            QStringList users;

            if ( modelsNeeding( eitem->data( 0, ROLE_GUID ).toString(),
                                users ) )
               needed << users;
         }
      }
   }

   if ( ! needed.isEmpty() )
   {
      QMessageBox::StandardButton answer = QMessageBox::question( this,
         tr( "Models Depend on These Edits" ),
         tr( "%1 selected model(s) need edits that you are about to\n"
             "deselect.\n\n"
             "Yes: drop those models from the bundle.\n"
             "No:  keep the raw data and edits selected." )
         .arg( needed.size() ),
         QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

      if ( answer == QMessageBox::No )  return;

      for ( int ii = 0; ii < needed.size(); ii++ )
         sel_models.removeAll( needed[ ii ] );

      buildModelTree();
   }

   updating = true;

   for ( int ii = 0; ii < tw_data->topLevelItemCount(); ii++ )
   {
      QTreeWidgetItem* ritem = tw_data->topLevelItem( ii );

      for ( int jj = 0; jj < ritem->childCount(); jj++ )
      {
         QTreeWidgetItem* witem = ritem->child( jj );
         witem->setCheckState( 0, Qt::Unchecked );

         for ( int kk = 0; kk < witem->childCount(); kk++ )
            witem->child( kk )->setCheckState( 0, Qt::Unchecked );
      }
   }

   updating = false;
   updateSummary();
}

void US_DataPubExportPane::setEditChecks( const QString& mode )
{
   updating = true;

   for ( int ii = 0; ii < tw_data->topLevelItemCount(); ii++ )
   {
      QTreeWidgetItem* ritem = tw_data->topLevelItem( ii );

      for ( int jj = 0; jj < ritem->childCount(); jj++ )
      {
         QTreeWidgetItem* witem = ritem->child( jj );
         int              knt   = witem->childCount();

         for ( int kk = 0; kk < knt; kk++ )
         {
            QTreeWidgetItem* eitem = witem->child( kk );
            bool             on    = false;

            if      ( mode == "all"    )  on = true;
            else if ( mode == "none"   )  on = false;
            else if ( mode == "latest" )  on = ( kk == knt - 1 );

            // An edit is only of use when its raw data travels with it
            if ( on  &&  witem->checkState( 0 ) != Qt::Checked )
               witem->setCheckState( 0, Qt::Checked );

            eitem->setCheckState( 0, on ? Qt::Checked : Qt::Unchecked );
         }
      }
   }

   updating = false;
   updateSummary();
}

void US_DataPubExportPane::latest_edits( void )
{
   setEditChecks( "latest" );
}

void US_DataPubExportPane::all_edits( void )
{
   setEditChecks( "all" );
}

void US_DataPubExportPane::no_edits( void )
{
   QStringList users;
   QStringList needed;

   for ( int ii = 0; ii < sel_models.size(); ii++ )
      needed << sel_models[ ii ];

   if ( ! needed.isEmpty() )
   {
      QMessageBox::StandardButton answer = QMessageBox::question( this,
         tr( "Models Depend on These Edits" ),
         tr( "%1 selected model(s) need the edits you are about to\n"
             "deselect.\n\n"
             "Yes: drop those models from the bundle.\n"
             "No:  keep the edits selected." ).arg( needed.size() ),
         QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

      if ( answer == QMessageBox::No )  return;

      sel_models.clear();
      sel_noises.clear();
      buildModelTree();
   }

   Q_UNUSED( users );

   setEditChecks( "none" );
}

bool US_DataPubExportPane::modelsNeeding( const QString& editGUID,
                                          QStringList& users ) const
{
   users.clear();

   for ( int ii = 0; ii < models.size(); ii++ )
   {
      if ( models[ ii ].editGUID != editGUID )            continue;
      if ( ! sel_models.contains( models[ ii ].guid ) )   continue;

      users << models[ ii ].guid;
   }

   return ! users.isEmpty();
}

void US_DataPubExportPane::item_changed( QTreeWidgetItem* item, int column )
{
   if ( updating  ||  item == nullptr  ||  column != 0 )  return;

   QString kind = item->data( 0, ROLE_KIND ).toString();
   QString guid = item->data( 0, ROLE_GUID ).toString();

   if ( kind == KIND_RAW  &&  item->checkState( 0 ) != Qt::Checked )
   {  // Dropping raw data drops the edits below it
      QStringList affected;

      for ( int ii = 0; ii < item->childCount(); ii++ )
      {
         QTreeWidgetItem* eitem = item->child( ii );

         if ( eitem->checkState( 0 ) != Qt::Checked )  continue;

         QStringList users;

         if ( modelsNeeding( eitem->data( 0, ROLE_GUID ).toString(), users ) )
            affected << users;
      }

      if ( ! affected.isEmpty() )
      {
         QMessageBox::StandardButton answer = QMessageBox::question( this,
            tr( "Models Depend on This Raw Data" ),
            tr( "%1 selected model(s) need edits of this raw data.\n\n"
                "Yes: drop those models from the bundle.\n"
                "No:  keep the raw data selected." ).arg( affected.size() ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

         if ( answer == QMessageBox::No )
         {
            updating = true;
            item->setCheckState( 0, Qt::Checked );
            updating = false;
            return;
         }

         for ( int ii = 0; ii < affected.size(); ii++ )
            sel_models.removeAll( affected[ ii ] );

         buildModelTree();
      }

      updating = true;

      for ( int ii = 0; ii < item->childCount(); ii++ )
         item->child( ii )->setCheckState( 0, Qt::Unchecked );

      updating = false;
   }

   else if ( kind == KIND_EDIT  &&  item->checkState( 0 ) == Qt::Checked )
   {  // An edit needs its raw data
      QTreeWidgetItem* witem = item->parent();

      if ( witem != nullptr  &&  witem->checkState( 0 ) != Qt::Checked )
      {
         updating = true;
         witem->setCheckState( 0, Qt::Checked );
         updating = false;
      }
   }

   else if ( kind == KIND_EDIT  &&  item->checkState( 0 ) != Qt::Checked )
   {
      QStringList users;

      if ( modelsNeeding( guid, users ) )
      {
         QMessageBox::StandardButton answer = QMessageBox::question( this,
            tr( "Models Depend on This Edit" ),
            tr( "%1 selected model(s) were fitted to this edit.\n\n"
                "Yes: drop those models from the bundle.\n"
                "No:  keep the edit selected." ).arg( users.size() ),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

         if ( answer == QMessageBox::No )
         {
            updating = true;
            item->setCheckState( 0, Qt::Checked );
            updating = false;
            return;
         }

         for ( int ii = 0; ii < users.size(); ii++ )
            sel_models.removeAll( users[ ii ] );

         buildModelTree();
      }
   }

   updateSummary();
}

// --------------------------------------------------------------- the models

void US_DataPubExportPane::select_models( void )
{
   if ( ! openCatalog() )  return;

   if ( runs.isEmpty() )
   {
      QMessageBox::information( this, tr( "No Runs" ),
         tr( "Select one or more experiments first." ) );
      return;
   }

   QString     error;
   QStringList runIDs;

   for ( int ii = 0; ii < runs.size(); ii++ )
      runIDs << runs[ ii ].runID;

   // The catalog knows which models belong to the runs; the loader lets the
   // user pick among them.
   models = catalog.models( runs, error );

   if ( ! error.isEmpty() )
      te_status->append( error );

   QString           mfilter;
   QList< US_Model > chosen;
   QStringList       descs;
   US_ModelLoader*   dialog = new US_ModelLoader( dkdb_cntrls->db(), mfilter,
                                                  chosen, descs, runIDs );
   dialog->exec();
   qApp->processEvents();
   delete dialog;

   if ( chosen.isEmpty() )  return;

   QStringList added;

   for ( int ii = 0; ii < chosen.size(); ii++ )
   {
      QString guid = chosen[ ii ].modelGUID;

      if ( guid.isEmpty()  ||  sel_models.contains( guid ) )  continue;

      sel_models << guid;
      added      << guid;

      // A model the catalog did not list is still exportable
      bool known = false;

      for ( int jj = 0; jj < models.size(); jj++ )
         if ( models[ jj ].guid == guid )  known = true;

      if ( known )  continue;

      US_DataPubCatalog::Model model;
      model.guid        = guid;
      model.description = chosen[ ii ].description;
      model.editGUID    = chosen[ ii ].editGUID;
      models << model;
   }

   // Make sure the edits the models were fitted to travel with them
   QStringList missing;

   for ( int ii = 0; ii < added.size(); ii++ )
   {
      QString editGUID;

      for ( int jj = 0; jj < models.size(); jj++ )
         if ( models[ jj ].guid == added[ ii ] )
            editGUID = models[ jj ].editGUID;

      if ( editGUID.isEmpty() )  continue;

      QTreeWidgetItem* eitem = itemFor( tw_data, editGUID );

      if ( eitem == nullptr )
      {
         missing << editGUID;
         continue;
      }

      if ( eitem->checkState( 0 ) != Qt::Checked )
         missing << editGUID;
   }

   missing.removeDuplicates();

   if ( ! missing.isEmpty() )
   {
      QMessageBox::StandardButton answer = QMessageBox::question( this,
         tr( "Select the Models' Edits?" ),
         tr( "%1 edit(s) that the selected models were fitted to are not\n"
             "selected yet.  A model cannot be imported without its edit.\n\n"
             "Yes: select those edits, and their raw data, as well.\n"
             "No:  leave the selection as it is." ).arg( missing.size() ),
         QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes );

      if ( answer == QMessageBox::Yes )
      {
         updating = true;

         for ( int ii = 0; ii < missing.size(); ii++ )
         {
            QTreeWidgetItem* eitem = itemFor( tw_data, missing[ ii ] );

            if ( eitem == nullptr )  continue;

            eitem->setCheckState( 0, Qt::Checked );

            if ( eitem->parent() != nullptr )
               eitem->parent()->setCheckState( 0, Qt::Checked );
         }

         updating = false;
      }
   }

   select_noises();
}

void US_DataPubExportPane::select_noises( void )
{
   if ( sel_models.isEmpty() )
   {
      buildModelTree();
      updateSummary();
      return;
   }

   if ( ! openCatalog() )  return;

   QList< US_DataPubCatalog::Model > chosen;

   for ( int ii = 0; ii < models.size(); ii++ )
      if ( sel_models.contains( models[ ii ].guid ) )
         chosen << models[ ii ];

   QString error;
   noises = catalog.noises( chosen, error );

   if ( ! error.isEmpty() )
      te_status->append( error );

   sel_noises.clear();

   for ( int ii = 0; ii < noises.size(); ii++ )
      sel_noises << noises[ ii ].guid;

   if ( noises.isEmpty() )
   {
      buildModelTree();
      updateSummary();
      return;
   }

   if ( US_Settings::noise_dialog() == 0 )
   {  // Auto-select: every noise record of every selected model travels
      te_status->append( tr( "%1 noise record(s) selected automatically." )
                         .arg( noises.size() ) );
      buildModelTree();
      updateSummary();
      return;
   }

   // The noise-dialog flag is set, so let the user choose
   QStringList mieGUIDs;
   QStringList nieGUIDs;

   for ( int ii = 0; ii < chosen.size(); ii++ )
      mieGUIDs << chosen[ ii ].guid;

   for ( int ii = 0; ii < noises.size(); ii++ )
   {
      int modelx = mieGUIDs.indexOf( noises[ ii ].modelGUID );

      if ( modelx < 0 )  continue;

      nieGUIDs << QString( "%1:%2:%3" ).arg( noises[ ii ].guid )
                  .arg( noises[ ii ].noiseType.isEmpty() ? QString( "ti" )
                                                         : noises[ ii ].noiseType )
                  .arg( modelx, 4, 10, QChar( '0' ) );
   }

   if ( nieGUIDs.isEmpty() )
   {
      buildModelTree();
      updateSummary();
      return;
   }

   US_Noise ti_noise;
   US_Noise ri_noise;
   US_NoiseLoader* dialog = new US_NoiseLoader( catalog.db(), mieGUIDs,
                                                nieGUIDs, ti_noise, ri_noise );
   dialog->exec();
   qApp->processEvents();
   delete dialog;

   QStringList picked;

   if ( ti_noise.count > 0  &&  ! ti_noise.noiseGUID.isEmpty() )
      picked << ti_noise.noiseGUID;

   if ( ri_noise.count > 0  &&  ! ri_noise.noiseGUID.isEmpty() )
      picked << ri_noise.noiseGUID;

   if ( ! picked.isEmpty() )
      sel_noises = picked;

   buildModelTree();
   updateSummary();
}

void US_DataPubExportPane::clear_models( void )
{
   sel_models.clear();
   sel_noises.clear();
   buildModelTree();
   updateSummary();
}

// -------------------------------------------------------- summary and export

US_DataPubExporter::Selection US_DataPubExportPane::selection( void ) const
{
   US_DataPubExporter::Selection sel;
   sel.fromDb           = dkdb_cntrls->db();
   sel.scope            = US_DataPub::scopeOfKey( cb_scope->currentText() );
   sel.projectGUID      = project_guid;
   sel.includeTimeState = ck_tmst->isChecked();
   sel.comment          = le_comment->text();

   for ( int ii = 0; ii < runs.size(); ii++ )
      sel.runIDs << runs[ ii ].runID;

   sel.setRaws  ( checkedRaws () );
   sel.setEdits ( checkedEdits() );
   sel.setModels( sel_models );
   sel.setNoises( sel_noises );

   return sel;
}

void US_DataPubExportPane::updateSummary( void )
{
   int nraws  = checkedRaws ().size();
   int nedits = checkedEdits().size();
   int nmods  = sel_models.size();
   int nnois  = sel_noises.size();

   le_summary->setText( tr( "%1 experiment(s), %2 raw data, %3 edit(s),"
                            " %4 model(s), %5 noise record(s)" )
                        .arg( runs.size() ).arg( nraws ).arg( nedits )
                        .arg( nmods ).arg( nnois ) );

   pb_export->setEnabled( ! runs.isEmpty()  ||  ! project_guid.isEmpty() );
}

void US_DataPubExportPane::show_details( void )
{
   US_DataPubExporter exporter;
   QString            error;

   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );
   bool ok = exporter.previewManifest( selection(), error );
   QApplication::restoreOverrideCursor();

   if ( ! ok )
   {
      QMessageBox::warning( this, tr( "Preview Problem" ), error );
      return;
   }

   QString text = tr( "Manifest preview -- payload digests are filled in"
                      " when the bundle is written.\n\n" )
                  + exporter.manifest().summary() + "\n"
                  + exporter.manifest().toYaml();

   US_Editor* editor = new US_Editor( US_Editor::DEFAULT, true, QString(),
                                      this );
   editor->setWindowTitle( tr( "Bundle Manifest Preview" ) );
   editor->resize( 720, 640 );
   editor->e->setFont( QFont( US_Widgets::fixedFont().family(),
                              US_GuiSettings::fontSize() ) );
   editor->e->setText( text );
   editor->show();
}

void US_DataPubExportPane::browse_bundle( void )
{
   QString start = le_bundle->text();

   if ( start.isEmpty() )
   {
      QString base = project_desc.isEmpty()
                     ? QString( "us3_bundle" )
                     : QString( project_desc ).replace(
                         QRegularExpression( "[^A-Za-z0-9_.-]" ), "_" );
      start = US_Settings::archiveDir() + "/" + base + ".tar.gz";
   }

   QString path = QFileDialog::getSaveFileName( this,
                  tr( "Write Data Publication Bundle" ), start,
                  tr( "Bundles (*.tar.gz);;All files (*)" ) );

   if ( path.isEmpty() )  return;

   if ( ! path.endsWith( ".tar.gz" ) )
      path += ".tar.gz";

   le_bundle->setText( path );
}

void US_DataPubExportPane::exporter_note( const QString& text )
{
   te_status->append( text );
   qApp->processEvents();
}

void US_DataPubExportPane::exporter_steps( int steps )
{
   pgb_progress->setMaximum( qMax( 1, steps ) );
}

void US_DataPubExportPane::exporter_step( int step )
{
   if ( pgb_progress->maximum() < step )
      pgb_progress->setMaximum( step );

   pgb_progress->setValue( step );
   qApp->processEvents();
}

void US_DataPubExportPane::run_export( void )
{
   QString path = le_bundle->text().trimmed();

   if ( path.isEmpty() )
   {
      browse_bundle();
      path = le_bundle->text().trimmed();
   }

   if ( path.isEmpty() )  return;

   if ( QFile::exists( path ) )
   {
      QMessageBox::StandardButton answer = QMessageBox::question( this,
         tr( "Replace the Bundle?" ),
         tr( "%1 already exists.  Replace it?" ).arg( path ),
         QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

      if ( answer != QMessageBox::Yes )  return;
   }

   US_DataPubExporter::Selection sel = selection();

   if ( sel.fromDb )
      sel.dbPassword = password();

   US_DataPubExporter exporter;
   connect( &exporter, &US_DataPubExporter::message,
            this,      &US_DataPubExportPane::exporter_note );
   connect( &exporter, &US_DataPubExporter::steps,
            this,      &US_DataPubExportPane::exporter_steps );
   connect( &exporter, &US_DataPubExporter::stepDone,
            this,      &US_DataPubExportPane::exporter_step );

   te_status->clear();
   pgb_progress->setValue( 0 );
   pb_export->setEnabled( false );
   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );

   QString error;
   bool    ok = exporter.exportBundle( sel, path, error );

   QApplication::restoreOverrideCursor();
   pb_export->setEnabled( true );

   if ( ! ok )
   {
      te_status->append( tr( "Export failed: " ) + error );
      QMessageBox::critical( this, tr( "Export Failed" ), error );
      emit status( tr( "Export failed" ) );
      return;
   }

   pgb_progress->setValue( pgb_progress->maximum() );
   emit status( tr( "Exported %1 records to %2" )
                .arg( exporter.manifest().total() ).arg( path ) );

   QMessageBox::information( this, tr( "Export Complete" ),
      tr( "%1 records were written to\n%2\n\n%3" )
      .arg( exporter.manifest().total() ).arg( path )
      .arg( exporter.manifest().summary() ) );
}

void US_DataPubExportPane::reset( void )
{
   clear_project();
   clear_runs();
   le_bundle->clear();
   le_comment->clear();
   te_status->clear();
   pgb_progress->setValue( 0 );
   cb_scope->setCurrentIndex( cb_scope->count() - 1 );
   ck_tmst->setChecked( true );
   updateSummary();
}
