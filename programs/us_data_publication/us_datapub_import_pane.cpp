//! \file us_datapub_import_pane.cpp
#include "us_datapub_import_pane.h"
#include "us_datapub_manifest.h"

#include "us_gui_settings.h"
#include "us_project_gui.h"
#include "us_settings.h"
#include "us_passwd.h"
#include "us_editor.h"

// ------------------------------------------------------- the conflict dialog

US_DataPubConflictDialog::US_DataPubConflictDialog(
      US_DataPub::EntityType type, const QString& name, bool identical,
      const QString& details, const QString& newName, QWidget* parent )
   : US_WidgetsDialog( parent, Qt::WindowFlags() )
{
   result = US_DataPub::PolicyRename;

   setWindowTitle( tr( "Name Already Taken" ) );
   setPalette    ( US_GuiSettings::frameColor() );

   QVBoxLayout* main = new QVBoxLayout( this );
   main->setContentsMargins( 2, 2, 2, 2 );
   main->setSpacing        ( 2 );

   main->addWidget( us_banner(
      tr( "%1 \"%2\" already exists in the target" )
      .arg( US_DataPub::typeText( type ) ).arg( name ) ) );

   QLabel* lb_detail = us_label( details );
   lb_detail->setWordWrap( true );
   main->addWidget( lb_detail );

   main->addWidget( us_label( identical
      ? tr( "The two records match, so the one already in the target can be"
            " used for everything that depends on it." )
      : tr( "The two records differ, so the one already in the target cannot"
            " stand in for this one.  It will be imported under a new name;"
            " nothing in the target is changed." ) ) );

   QHBoxLayout* namelay = new QHBoxLayout;
   namelay->addWidget( us_label( tr( "Import under the name:" ) ) );
   le_name = us_lineedit( newName, 0, false );
   namelay->addWidget( le_name );

   // A run ID becomes a directory and file name, so only a usable one can
   // be typed
   if ( type == US_DataPub::Experiment )
      le_name->setValidator( new QRegularExpressionValidator(
                             QRegularExpression( "[A-Za-z0-9_-]{1,80}" ),
                             le_name ) );
   main->addLayout( namelay );

   QGridLayout* alllay = us_checkbox(
         tr( "Apply this answer to every further conflict of this type" ),
         ck_all, false );
   main->addLayout( alllay );

   QHBoxLayout* buttons = new QHBoxLayout;
   QPushButton* pb_reuse  = us_pushbutton( tr( "Use the Existing Record" ) );
   QPushButton* pb_rename = us_pushbutton( tr( "Import Under a New Name" ) );
   QPushButton* pb_cancel = us_pushbutton( tr( "Abort the Import" ) );
   pb_reuse->setEnabled( identical );
   buttons->addWidget( pb_reuse  );
   buttons->addWidget( pb_rename );
   buttons->addWidget( pb_cancel );
   main->addLayout( buttons );

   connect( pb_reuse,  &QPushButton::clicked,
            this,      &US_DataPubConflictDialog::chose_reuse );
   connect( pb_rename, &QPushButton::clicked,
            this,      &US_DataPubConflictDialog::chose_rename );
   connect( pb_cancel, &QPushButton::clicked,
            this,      &US_DataPubConflictDialog::chose_cancel );
}

US_DataPub::ConflictPolicy US_DataPubConflictDialog::policy( void ) const
{
   return result;
}

QString US_DataPubConflictDialog::chosenName( void ) const
{
   return le_name->text().trimmed();
}

bool US_DataPubConflictDialog::applyToAll( void ) const
{
   return ck_all->isChecked();
}

void US_DataPubConflictDialog::chose_reuse( void )
{
   result = US_DataPub::PolicyReuse;
   accept();
}

void US_DataPubConflictDialog::chose_rename( void )
{
   result = US_DataPub::PolicyRename;
   accept();
}

void US_DataPubConflictDialog::chose_cancel( void )
{
   result = US_DataPub::PolicyFail;
   reject();
}

// ----------------------------------------------------------- the import tab

US_DataPubImportPane::US_DataPubImportPane( QWidget* parent )
   : US_Widgets( true, parent )
{
   was_cancelled = false;

   setPalette( US_GuiSettings::frameColor() );

   QVBoxLayout* main = new QVBoxLayout( this );
   main->setContentsMargins( 2, 2, 2, 2 );
   main->setSpacing        ( 2 );

   dkdb_cntrls = new US_Disk_DB_Controls(
                 US_Settings::default_data_location() );
   state       = US_DataPubImportState( dkdb_cntrls->db() );
   connect( dkdb_cntrls, &US_Disk_DB_Controls::changed,
            this,        &US_DataPubImportPane::target_changed );
   main->addWidget( us_banner( tr( "Import data into" ) ) );
   main->addLayout( dkdb_cntrls );

   QGridLayout* pick = new QGridLayout;
   int row = 0;

   pb_browse  = us_pushbutton( tr( "Select Bundle..." ) );
   le_bundle  = us_lineedit( "", 0, false );
   pb_inspect = us_pushbutton( tr( "Inspect" ) );
   pick->addWidget( pb_browse,  row, 0, 1, 1 );
   pick->addWidget( le_bundle,  row, 1, 1, 3 );
   pick->addWidget( pb_inspect, row++, 4, 1, 1 );

   pb_outdir = us_pushbutton( tr( "Disk Target Folder..." ) );
   le_outdir = us_lineedit( "", 0, false );
   le_outdir->setPlaceholderText(
         tr( "empty: the UltraScan3 data and results directories" ) );
   pick->addWidget( pb_outdir, row, 0, 1, 1 );
   pick->addWidget( le_outdir, row++, 1, 1, 4 );

   // The receiving installation keeps its own project list, so the data can
   // be filed under a project of this user's choosing.
   pb_project   = us_pushbutton( tr( "Import into Project..." ) );
   le_project   = us_lineedit( "", 0, true );
   le_project->setPlaceholderText(
         tr( "empty: the project the bundle names" ) );
   pb_clearproj = us_pushbutton( tr( "Clear" ) );
   pick->addWidget( pb_project,   row, 0, 1, 1 );
   pick->addWidget( le_project,   row, 1, 1, 3 );
   pick->addWidget( pb_clearproj, row++, 4, 1, 1 );

   main->addLayout( pick );

   connect( pb_browse,  &QPushButton::clicked,
            this,       &US_DataPubImportPane::browse_bundle );
   connect( le_bundle,  &QLineEdit::textEdited,
            this,       &US_DataPubImportPane::bundle_edited );
   connect( pb_outdir,  &QPushButton::clicked,
            this,       &US_DataPubImportPane::browse_outdir );
   connect( pb_project, &QPushButton::clicked,
            this,       &US_DataPubImportPane::select_project );
   connect( pb_clearproj, &QPushButton::clicked,
            this,       &US_DataPubImportPane::clear_project );
   connect( pb_inspect, &QPushButton::clicked,
            this,       &US_DataPubImportPane::inspect );

   main->addWidget( us_banner( tr( "Bundle contents" ) ) );

   tw_bundle = new QTreeWidget( this );
   tw_bundle->setFrameStyle( QFrame::NoFrame );
   tw_bundle->setPalette   ( US_GuiSettings::editColor() );
   tw_bundle->setFont      ( QFont( US_GuiSettings::fontFamily(),
                                    US_GuiSettings::fontSize() ) );
   tw_bundle->setColumnCount( 3 );
   QStringList headers;
   headers << tr( "Record" ) << tr( "ID" ) << tr( "GUID" );
   tw_bundle->setHeaderLabels( headers );
   main->addWidget( tw_bundle );

   QGridLayout* opts = new QGridLayout;
   row = 0;

   opts->addWidget( us_label( tr( "On a name conflict:" ) ), row, 0, 1, 1 );
   cb_policy = us_comboBox();
   cb_policy->addItem( tr( "reuse the existing record when it matches" ),
                       US_DataPub::PolicyReuse );
   cb_policy->addItem( tr( "always import under a new name" ),
                       US_DataPub::PolicyRename );
   cb_policy->addItem( tr( "stop the import" ), US_DataPub::PolicyFail );
   opts->addWidget( cb_policy, row, 1, 1, 2 );

   QGridLayout* promptlay = us_checkbox( tr( "Ask me each time" ), ck_prompt,
                                         true );
   opts->addLayout( promptlay, row++, 3, 1, 2 );

   QGridLayout* verlay = us_checkbox( tr( "Verify payload digests" ),
                                      ck_verify, true );
   opts->addLayout( verlay, row, 0, 1, 2 );
   row++;

   opts->addWidget( us_label( tr( "Summary:" ) ), row, 0, 1, 1 );
   le_summary = us_lineedit( tr( "No bundle inspected" ), 0, true );
   pb_details = us_pushbutton( tr( "Details..." ) );
   opts->addWidget( le_summary, row, 1, 1, 3 );
   opts->addWidget( pb_details, row++, 4, 1, 1 );

   main->addLayout( opts );

   connect( pb_details, &QPushButton::clicked,
            this,       &US_DataPubImportPane::show_details );

   pgb_progress = us_progressBar( 0, 100, 0 );
   main->addWidget( pgb_progress );

   te_status = us_textedit();
   te_status->setReadOnly( true );
   te_status->setMaximumHeight( 120 );
   main->addWidget( te_status );

   QHBoxLayout* buttons = new QHBoxLayout;
   QPushButton* pb_help = us_pushbutton( tr( "Help" ) );
   pb_reset  = us_pushbutton( tr( "Reset" ) );
   pb_import = us_pushbutton( tr( "Import Bundle" ) );
   pb_import->setEnabled( false );
   buttons->addWidget( pb_help );
   buttons->addWidget( pb_reset );
   buttons->addWidget( pb_import );
   main->addLayout( buttons );

   connect( pb_help,   &QPushButton::clicked,
            this,      &US_DataPubImportPane::help );
   connect( pb_reset,  &QPushButton::clicked,
            this,      &US_DataPubImportPane::reset );
   connect( pb_import, &QPushButton::clicked,
            this,      &US_DataPubImportPane::run_import );

   connect( &importer, &US_DataPubImporter::message,
            this,      &US_DataPubImportPane::importer_note );
   connect( &importer, &US_DataPubImporter::steps,
            this,      &US_DataPubImportPane::importer_steps );
   connect( &importer, &US_DataPubImporter::stepDone,
            this,      &US_DataPubImportPane::importer_step );

   importer.setResolver( this );
   refreshControls();
}

void US_DataPubImportPane::target_changed( bool db )
{
   // A project was chosen from the other target's list; it means nothing in
   // this one.  Only a real change of target clears it.
   if ( state.setTarget( db ) )
   {
      le_project->clear();
      te_status->append( tr( "The chosen project belongs to the other target"
                             " and was cleared" ) );
   }

   refreshControls();
}

// A bundle path typed after an inspection is not the bundle inspected
void US_DataPubImportPane::bundle_edited( const QString& )
{
   if ( ! state.inspected() )  return;

   state.setInspected( false );
   refreshControls();
   tw_bundle->clear();
   le_summary->setText( tr( "No bundle inspected" ) );
}

bool US_DataPubImportPane::isBusy( void ) const
{
   return state.busy();
}

bool US_DataPubImportPane::cancelled( void ) const
{
   return was_cancelled;
}

/* While an import runs the importer hands control back to the event loop
   to show its progress, so everything that could change or destroy what it
   works on -- the bundle, the target, the options, Reset, Inspect -- is
   switched off; US_DataPublication asks isBusy() before it closes.
*/
void US_DataPubImportPane::setBusy( bool on )
{
   if ( on == state.busy() )  return;

   state.setBusy( on );
   refreshControls();

   emit busyChanged( on );
}

/* What the controls allow, from the state alone.  This is all the end of
   an import or an inspection does: it does not go through target_changed(),
   which would take the end of every import for a change of target and
   clear the chosen project.
*/
void US_DataPubImportPane::refreshControls( void )
{
   bool idle = state.controlsEnabled();

   QList< QWidget* > widgets;
   widgets << pb_browse << pb_inspect << pb_project << pb_clearproj
           << pb_details << pb_reset << le_bundle << cb_policy
           << ck_verify << ck_prompt << tw_bundle;

   for ( int ii = 0; ii < dkdb_cntrls->count(); ii++ )
   {
      QWidget* widget = dkdb_cntrls->itemAt( ii )->widget();

      if ( widget != nullptr )  widgets << widget;
   }

   for ( int ii = 0; ii < widgets.size(); ii++ )
      widgets[ ii ]->setEnabled( idle );

   pb_outdir->setEnabled( state.outputDirEnabled() );
   le_outdir->setEnabled( state.outputDirEnabled() );
   pb_import->setEnabled( state.importEnabled() );
}

void US_DataPubImportPane::browse_bundle( void )
{
   QString path = QFileDialog::getOpenFileName( this,
                  tr( "Read Data Publication Bundle" ),
                  US_Settings::archiveDir(),
                  tr( "Bundles (*.tar.gz *.tgz);;All files (*)" ) );

   if ( path.isEmpty() )  return;

   le_bundle->setText( path );
   inspect();
}

/* Pick the project of the target the imported runs should belong to.

   A bundle names the project its runs came from, but that is the exporting
   installation's project.  Choosing one here files the data under a project
   this installation already has, and the bundle's own project record is
   then reused instead of created.
*/
void US_DataPubImportPane::select_project( void )
{
   if ( state.busy() )  return;

   int where = dkdb_cntrls->db() ? US_Disk_DB_Controls::DB
                                 : US_Disk_DB_Controls::Disk;
   US_ProjectGui* dialog = new US_ProjectGui( true, where );

   connect( dialog, &US_ProjectGui::updateProjectGuiSelection,
            this,   &US_DataPubImportPane::project_chosen );

   dialog->exec();
   qApp->processEvents();

   delete dialog;
}

void US_DataPubImportPane::project_chosen( US_Project& project )
{
   state.setProject( project.projectGUID, project.projectDesc );

   le_project->setText( QString( "%1  [%2]" ).arg( state.projectDesc() )
                        .arg( state.projectGUID() ) );

   emit status( tr( "The imported runs will be attached to \"%1\"" )
                .arg( state.projectDesc() ) );
}

void US_DataPubImportPane::clear_project( void )
{
   state.clearProject();
   le_project->clear();
}

void US_DataPubImportPane::browse_outdir( void )
{
   QString path = QFileDialog::getExistingDirectory( this,
                  tr( "Disk Target Folder" ),
                  le_outdir->text().isEmpty() ? US_Settings::workBaseDir()
                                              : le_outdir->text() );

   if ( path.isEmpty() )  return;

   le_outdir->setText( path );
}

/* An inspection unpacks and reads a bundle, and the notes it makes hand
   control to the event loop; like an import it is busy meanwhile, so that
   it cannot be started again, or an import started, while it runs.
*/
void US_DataPubImportPane::inspect( void )
{
   if ( state.busy() )  return;

   if ( le_bundle->text().trimmed().isEmpty() )
   {
      browse_bundle();
      return;
   }

   setBusy( true );
   inspectBundle();
   setBusy( false );
}

void US_DataPubImportPane::inspectBundle( void )
{
   QString path = le_bundle->text().trimmed();

   te_status->clear();
   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );

   QString error;
   bool    ok = importer.inspect( path, error );

   QApplication::restoreOverrideCursor();

   state.setInspected( ok );

   if ( ! ok )
   {
      tw_bundle->clear();
      le_summary->setText( tr( "The bundle could not be read" ) );
      QMessageBox::warning( this, tr( "Bundle Problem" ), error );
      return;
   }

   QStringList problems = importer.verifyPayloads();

   if ( ! problems.isEmpty() )
   {
      te_status->append( tr( "Payload problems:" ) );

      for ( int ii = 0; ii < problems.size(); ii++ )
         te_status->append( "  " + problems[ ii ] );
   }

   buildTree();
   updateSummary();
}

void US_DataPubImportPane::buildTree( void )
{
   tw_bundle->clear();

   const US_DataPubManifest& mani = importer.manifest();
   QList< US_DataPub::EntityType > types = US_DataPubManifest::orderedTypes();

   for ( int ii = 0; ii < types.size(); ii++ )
   {
      QList< US_DataPubEntity > entities = mani.section( types[ ii ] );

      if ( entities.isEmpty() )  continue;

      QStringList ttext;
      ttext << tr( "%1 (%2)" ).arg( US_DataPub::typeText( types[ ii ] ) )
                              .arg( entities.size() );
      QTreeWidgetItem* titem = new QTreeWidgetItem( ttext );
      tw_bundle->addTopLevelItem( titem );

      for ( int jj = 0; jj < entities.size(); jj++ )
      {
         const US_DataPubEntity& entity = entities[ jj ];
         QStringList etext;
         etext << ( entity.name.isEmpty() ? entity.filename : entity.name )
               << entity.id
               << entity.guid;
         titem->addChild( new QTreeWidgetItem( etext ) );
      }

      titem->setExpanded( entities.size() <= 12 );
   }

   for ( int ii = 0; ii < 3; ii++ )
      tw_bundle->resizeColumnToContents( ii );
}

void US_DataPubImportPane::updateSummary( void )
{
   const US_DataPubManifest& mani = importer.manifest();

   le_summary->setText( tr( "%1 records, scope \"%2\", exported from the %3"
                            " on %4" )
                        .arg( mani.total() ).arg( mani.scope )
                        .arg( mani.source ).arg( mani.created ) );
}

void US_DataPubImportPane::show_details( void )
{
   if ( state.busy() )  return;

   if ( ! state.inspected() )
   {
      QMessageBox::information( this, tr( "No Bundle" ),
         tr( "Inspect a bundle first." ) );
      return;
   }

   US_Editor* editor = new US_Editor( US_Editor::DEFAULT, true, QString(),
                                      this );
   editor->setWindowTitle( tr( "Bundle Manifest" ) );
   editor->resize( 720, 640 );
   editor->e->setFont( QFont( US_Widgets::fixedFont().family(),
                              US_GuiSettings::fontSize() ) );
   editor->e->setText( importer.manifest().toYaml() );
   editor->show();
}

US_DataPub::ConflictPolicy US_DataPubImportPane::resolve(
      US_DataPub::EntityType type, const QString& name, bool identical,
      const QString& details, QString& newName )
{
   if ( sticky.contains( int( type ) ) )
      return sticky.value( int( type ) );

   if ( ! ck_prompt->isChecked() )
      return US_DataPub::ConflictPolicy(
             cb_policy->currentData().toInt() );

   US_DataPubConflictDialog dialog( type, name, identical, details, newName,
                                    this );
   int answer = dialog.exec();

   // Escape, the close button and "Abort" all end the import as cancelled
   if ( answer != QDialog::Accepted )
   {
      was_cancelled = true;
      return US_DataPub::PolicyFail;
   }

   if ( ! dialog.chosenName().isEmpty() )
      newName = dialog.chosenName();

   if ( dialog.applyToAll() )
      sticky.insert( int( type ), dialog.policy() );

   return dialog.policy();
}

void US_DataPubImportPane::importer_note( const QString& text )
{
   te_status->append( text );
   qApp->processEvents();
}

void US_DataPubImportPane::importer_steps( int steps )
{
   pgb_progress->setMaximum( qMax( 1, steps ) );
}

void US_DataPubImportPane::importer_step( int step )
{
   if ( pgb_progress->maximum() < step )
      pgb_progress->setMaximum( step );

   pgb_progress->setValue( step );
   qApp->processEvents();
}

void US_DataPubImportPane::run_import( void )
{
   if ( state.busy() )  return;

   if ( ! state.inspected() )
   {
      inspect();

      if ( ! state.inspected() )  return;
   }

   US_DataPubImporter::Options options;
   options.target       = dkdb_cntrls->db() ? US_DataPub::TargetDb
                                            : US_DataPub::TargetDisk;
   options.outputDir    = le_outdir->text().trimmed();
   options.projectGUID  = state.projectGUID();
   options.verifyHashes = ck_verify->isChecked();
   options.policy       = US_DataPub::ConflictPolicy(
                          cb_policy->currentData().toInt() );

   if ( options.target == US_DataPub::TargetDb )
   {
      US_Passwd pw;
      options.dbPassword = pw.getPasswd();

      if ( options.dbPassword.isEmpty() )
      {
         te_status->append( tr( "Import cancelled: no master password was"
                                " given, so the database cannot be opened" ) );
         emit status( tr( "Import cancelled" ) );
         return;
      }
   }

   sticky.clear();
   was_cancelled = false;
   te_status->clear();
   pgb_progress->setValue( 0 );
   setBusy( true );
   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );

   QString error;
   bool    ok = importer.runImport( options, error );

   QApplication::restoreOverrideCursor();

   // The tab stays busy until the result has been seen: the window and the
   // other tab come back only when the dialogs below are closed
   report_import( ok, error );
   setBusy( false );
}

void US_DataPubImportPane::report_import( bool ok, const QString& error )
{
   QStringList warnings = importer.warnings();

   if ( ! ok )
   {
      QString title = was_cancelled ? tr( "Import Cancelled" )
                                    : tr( "Import Failed" );
      QString text  = error;

      if ( ! importer.outcome().isEmpty() )
         text += "\n\n" + importer.outcome();

      te_status->append( title + ": " + text );
      QMessageBox::critical( this, title, text );
      emit status( title );
      return;
   }

   QList< US_DataPubImporter::Result > results = importer.results();
   int counts[ 8 ] = { 0, 0, 0, 0, 0, 0, 0, 0 };

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      int res = int( results[ ii ].resolution );

      if ( res >= 0  &&  res < 8 )  counts[ res ]++;
   }

   pgb_progress->setValue( pgb_progress->maximum() );

   QString tally = tr( "%1 created, %2 reused, %3 renamed, %4 substituted,"
                       " %5 not imported" )
                   .arg( counts[ US_DataPub::ResolvedCreated     ] )
                   .arg( counts[ US_DataPub::ResolvedReused      ] )
                   .arg( counts[ US_DataPub::ResolvedRenamed     ] )
                   .arg( counts[ US_DataPub::ResolvedSubstituted ] )
                   .arg( counts[ US_DataPub::ResolvedOmitted     ] );
   QString summary = tr( "%1 records: %2" ).arg( results.size() ).arg( tally );

   emit status( summary );

   if ( warnings.isEmpty() )
   {
      QMessageBox::information( this, tr( "Import Complete" ), summary );
      return;
   }

   QMessageBox::warning( this, tr( "Import Complete, With Warnings" ),
                         summary + "\n\n" + warnings.join( "\n" ) );
}

void US_DataPubImportPane::reset( void )
{
   if ( state.busy() )  return;

   clear_project();
   state.setInspected( false );
   sticky.clear();
   tw_bundle->clear();
   te_status->clear();
   le_bundle->clear();
   le_outdir->clear();
   le_summary->setText( tr( "No bundle inspected" ) );
   pgb_progress->setValue( 0 );
   refreshControls();
   ck_verify->setChecked( true );
   ck_prompt->setChecked( true );
}
