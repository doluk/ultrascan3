//! \file us_datapub_import_pane.cpp
#include "us_datapub_import_pane.h"
#include "us_datapub_manifest.h"

#include "us_gui_settings.h"
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
   inspected = false;

   setPalette( US_GuiSettings::frameColor() );

   QVBoxLayout* main = new QVBoxLayout( this );
   main->setContentsMargins( 2, 2, 2, 2 );
   main->setSpacing        ( 2 );

   dkdb_cntrls = new US_Disk_DB_Controls(
                 US_Settings::default_data_location() );
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

   main->addLayout( pick );

   connect( pb_browse,  &QPushButton::clicked,
            this,       &US_DataPubImportPane::browse_bundle );
   connect( pb_outdir,  &QPushButton::clicked,
            this,       &US_DataPubImportPane::browse_outdir );
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

   QGridLayout* drylay = us_checkbox( tr( "Dry run (write nothing)" ),
                                      ck_dryrun, false );
   QGridLayout* verlay = us_checkbox( tr( "Verify payload digests" ),
                                      ck_verify, true );
   opts->addLayout( drylay, row, 0, 1, 2 );
   opts->addLayout( verlay, row, 2, 1, 2 );
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
}

void US_DataPubImportPane::target_changed( bool db )
{
   pb_outdir->setEnabled( ! db );
   le_outdir->setEnabled( ! db );
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

void US_DataPubImportPane::browse_outdir( void )
{
   QString path = QFileDialog::getExistingDirectory( this,
                  tr( "Disk Target Folder" ),
                  le_outdir->text().isEmpty() ? US_Settings::workBaseDir()
                                              : le_outdir->text() );

   if ( path.isEmpty() )  return;

   le_outdir->setText( path );
}

void US_DataPubImportPane::inspect( void )
{
   QString path = le_bundle->text().trimmed();

   if ( path.isEmpty() )
   {
      browse_bundle();
      return;
   }

   te_status->clear();
   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );

   QString error;
   bool    ok = importer.inspect( path, error );

   QApplication::restoreOverrideCursor();

   inspected = ok;
   pb_import->setEnabled( ok );

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
   if ( ! inspected )
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

   if ( answer != QDialog::Accepted )
      return US_DataPub::PolicyFail;

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
   if ( ! inspected )
   {
      inspect();

      if ( ! inspected )  return;
   }

   US_DataPubImporter::Options options;
   options.target       = dkdb_cntrls->db() ? US_DataPub::TargetDb
                                            : US_DataPub::TargetDisk;
   options.outputDir    = le_outdir->text().trimmed();
   options.dryRun       = ck_dryrun->isChecked();
   options.verifyHashes = ck_verify->isChecked();
   options.policy       = US_DataPub::ConflictPolicy(
                          cb_policy->currentData().toInt() );

   if ( options.target == US_DataPub::TargetDb )
   {
      US_Passwd pw;
      options.dbPassword = pw.getPasswd();
   }

   sticky.clear();
   te_status->clear();
   pgb_progress->setValue( 0 );
   pb_import->setEnabled( false );
   QApplication::setOverrideCursor( QCursor( Qt::WaitCursor ) );

   QString error;
   bool    ok = importer.runImport( options, error );

   QApplication::restoreOverrideCursor();
   pb_import->setEnabled( true );

   if ( ! ok )
   {
      te_status->append( tr( "Import failed: " ) + error );
      QMessageBox::critical( this, tr( "Import Failed" ), error );
      emit status( tr( "Import failed" ) );
      return;
   }

   QList< US_DataPubImporter::Result > results = importer.results();
   int created = 0;
   int reused  = 0;
   int renamed = 0;

   for ( int ii = 0; ii < results.size(); ii++ )
   {
      switch ( results[ ii ].resolution )
      {
         case US_DataPub::ResolvedCreated:  created++;  break;
         case US_DataPub::ResolvedReused:   reused++;   break;
         case US_DataPub::ResolvedRenamed:  renamed++;  break;
         default:                                       break;
      }
   }

   pgb_progress->setValue( pgb_progress->maximum() );

   QString summary = options.dryRun
      ? tr( "Dry run: %1 records would be created, %2 reused, %3 renamed" )
        .arg( created ).arg( reused ).arg( renamed )
      : tr( "Imported %1 records: %2 created, %3 reused, %4 renamed" )
        .arg( results.size() ).arg( created ).arg( reused ).arg( renamed );

   emit status( summary );
   QMessageBox::information( this, tr( "Import Complete" ), summary );
}

void US_DataPubImportPane::reset( void )
{
   inspected = false;
   sticky.clear();
   tw_bundle->clear();
   te_status->clear();
   le_bundle->clear();
   le_outdir->clear();
   le_summary->setText( tr( "No bundle inspected" ) );
   pgb_progress->setValue( 0 );
   pb_import->setEnabled( false );
   ck_dryrun->setChecked( false );
   ck_verify->setChecked( true );
   ck_prompt->setChecked( true );
}
