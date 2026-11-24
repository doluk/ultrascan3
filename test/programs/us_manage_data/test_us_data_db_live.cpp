// test_us_data_db_live.cpp - us_manage_data's removals against a real database
//
// These need a MySQL/MariaDB server with the US3 schema, procedures and the
// test fixture (test/utils/data/us3_db_fixture.sql), and write to it: point
// them at a copy.  They log in as the fixture's ordinary investigator and as
// its administrator, so they need both:
//
//   US3_TEST_DB_HOST=127.0.0.1:3306 US3_TEST_DB_NAME=<a copy of us3test>
//   US3_TEST_DB_USER=us3test        US3_TEST_DB_PASS=...
//   US3_TEST_DB_PERSON_GUID=... US3_TEST_DB_PERSON_PW=... US3_TEST_DB_PERSON_ID=2
//   US3_TEST_DB_ADMIN_GUID=...  US3_TEST_DB_ADMIN_PW=...  US3_TEST_DB_OTHER_ID=3
//
// Without them every test here skips.  The same paths run against the fake
// server of test_us_data_db.cpp; this is what that fake is checked against.
#include <gtest/gtest.h>

#include <QtCore>
#include <QProgressBar>
#include <QLabel>
#include <QTreeWidget>

#include "us_data_model.h"
#include "us_data_process.h"
#include "us_data_tree.h"
#include "us_db2.h"
#include "us_convertio.h"
#include "us_settings.h"
#include "us_util.h"

class TestUSDataDbLive : public ::testing::Test
{
   protected:
      US_DB2* inv      = nullptr;       // the ordinary investigator
      US_DB2* admin    = nullptr;       // the administrator
      int     personID = 0;
      int     otherID  = 0;
      int     savedInv = 0;
      QString savedWorkDir;
      QString root;

      const QString runID    = "MWL_demo_run";
      const QString rawGUID  = "aaaaaaaa-0000-4000-8000-000000000100";
      const QString othEdit  = "aaaaaaaa-0000-4000-8000-000000000210";

      // What a test wrote, removed again at its end
      QStringList cleanup;

      void SetUp() override
      {
         QString host  = qEnvironmentVariable( "US3_TEST_DB_HOST" );
         QString name  = qEnvironmentVariable( "US3_TEST_DB_NAME" );
         QString user  = qEnvironmentVariable( "US3_TEST_DB_USER" );
         QString pass  = qEnvironmentVariable( "US3_TEST_DB_PASS" );
         QString guid  = qEnvironmentVariable( "US3_TEST_DB_PERSON_GUID" );
         QString pw    = qEnvironmentVariable( "US3_TEST_DB_PERSON_PW" );
         QString aguid = qEnvironmentVariable( "US3_TEST_DB_ADMIN_GUID" );
         QString apw   = qEnvironmentVariable( "US3_TEST_DB_ADMIN_PW" );
         personID      = qEnvironmentVariable( "US3_TEST_DB_PERSON_ID" ).toInt();
         otherID       = qEnvironmentVariable( "US3_TEST_DB_OTHER_ID" ).toInt();

         if ( host.isEmpty()  ||  name.isEmpty()  ||  user.isEmpty()  ||
              guid.isEmpty()  ||  aguid.isEmpty()  ||  personID <= 0  ||
              otherID <= 0 )
            GTEST_SKIP() << "no test database configured, with an administrator";

         QString error;
         inv   = new US_DB2();
         admin = new US_DB2();

         if ( ! inv  ->connectAuthenticated( host, name, user, pass, guid,  pw,  error )  ||
              ! admin->connectAuthenticated( host, name, user, pass, aguid, apw, error ) )
            GTEST_SKIP() << "cannot reach the test database: " << error.toStdString();

         savedInv     = US_Settings::us_inv_ID();
         savedWorkDir = US_Settings::workBaseDir();
         US_Settings::set_us_inv_ID( personID );

         root = QDir::tempPath() + "/us_data_db_live_"
                + QString::number( QCoreApplication::applicationPid() );
         QDir().mkpath( root );
         US_Settings::set_workBaseDir( root );
      }

      void TearDown() override
      {
         // What is left of what the test wrote, in an order the
         // constraints allow
         for ( int ii = 0; admin != nullptr  &&  ii < cleanup.size(); ii++ )
            admin->statusQuery( cleanup[ ii ].split( " " ) );

         delete inv;
         delete admin;
         inv   = nullptr;
         admin = nullptr;

         if ( ! root.isEmpty() )
         {
            US_Settings::set_us_inv_ID( savedInv );
            US_Settings::set_workBaseDir( savedWorkDir );
            QDir( root ).removeRecursively();
         }
      }

      int idOf( US_DB2* db, const QString& proc, const QString& guid )
      {
         db->query( QStringList() << proc << guid );
         return ( db->lastErrno() == IUS_DB2::OK  &&  db->next() )
                ? db->value( 0 ).toInt() : -1;
      }

      // 1 present, 0 gone, -1 when the server did not say
      int present( const QString& proc, int id, int gone )
      {
         admin->query( QStringList() << proc << QString::number( id ) );
         int status = admin->lastErrno();

         if ( status == IUS_DB2::OK )                          return admin->next() ? 1 : 0;
         if ( status == gone  ||  status == IUS_DB2::NOROWS )  return 0;
         return -1;
      }

      // A model of an edit, by GUID, owned by someone
      int newModel( US_DB2* db, const QString& guid, const QString& editGUID,
                    const QString& desc, int owner )
      {
         int st = db->statusQuery( QStringList() << "new_model" << guid << desc
                                   << "<ModelData/>" << "0.001" << "5.9"
                                   << editGUID << QString::number( owner ) );
         EXPECT_EQ( st, IUS_DB2::OK ) << db->lastError().toStdString();
         return idOf( db, "get_modelID", guid );
      }

      int newNoise( US_DB2* db, const QString& guid, int editID,
                    const QString& modelGUID, const QString& desc )
      {
         int st = db->statusQuery( QStringList() << "new_noise" << guid
                                   << QString::number( editID ) << "0" << modelGUID
                                   << "ti_noise" << desc << "<NoiseData/>" );
         EXPECT_EQ( st, IUS_DB2::OK ) << db->lastError().toStdString();
         return idOf( db, "get_noiseID", guid );
      }

      // An edit of the investigator's run, with a model and its noise of
      // theirs, and noise of another investigator's model of another edit
      // recorded against it
      struct Edit
      {
         QString editG, modelG, noiseG, theirM, theirN;
         int     editID = -1, modelID = -1, noiseID = -1, othM = -1, othN = -1;
      };

      void newEdit( Edit& ed )
      {
         QString stamp = QDateTime::currentDateTimeUtc().toString( "yyMMddhhmmss" );
         ed.editG      = US_Util::new_guid();

         int rawID = -1;
         QString error;
         ASSERT_EQ( US_ConvertIO::findRawDataId( inv, rawGUID, rawID, error ), IUS_DB2::OK )
            << error.toStdString();

         US_ConvertIO::EditedDataRecord record;
         record.rawDataID = rawID;
         record.editGUID  = ed.editG;
         record.label     = runID;
         record.filename  = runID + "." + stamp.left( 10 ) + ".RA.1.A.280.xml";
         record.comment   = "live removal test";

         ASSERT_EQ( US_ConvertIO::createEditedData( inv, record, ed.editID, error ), IUS_DB2::OK )
            << error.toStdString();
         cleanup << "delete_editedData " + QString::number( ed.editID );

         QString base = runID + ".1A280.e" + stamp.left( 10 );
         ed.modelG    = US_Util::new_guid();
         ed.noiseG    = US_Util::new_guid();
         ed.theirM    = US_Util::new_guid();
         ed.theirN    = US_Util::new_guid();

         ed.modelID = newModel( inv, ed.modelG, ed.editG, base + "_a1_2DSA_i01.model", personID );
         ed.noiseID = newNoise( inv, ed.noiseG, ed.editID, ed.modelG, base + "_a1_2DSA_i01.ti_noise" );
         ed.othM    = newModel( admin, ed.theirM, othEdit, "Other_run.1A280.global.model", otherID );
         ed.othN    = newNoise( admin, ed.theirN, ed.editID, ed.theirM, "Other_run.1A280.global.ti_noise" );
         ASSERT_GT( ed.modelID, 0 );
         ASSERT_GT( ed.noiseID, 0 );
         ASSERT_GT( ed.othM, 0 );
         ASSERT_GT( ed.othN, 0 );
         cleanup.prepend( "delete_model " + QString::number( ed.othM ) );
         cleanup.prepend( "delete_noise " + QString::number( ed.othN ) );
      }

      // The row of a record of the run, by GUID
      static int rowOf( US_DataModel& model, const QString& guid )
      {
         for ( int row = 0; row < model.recCount(); row++ )
            if ( model.row_datadesc( row ).dataGUID.compare( guid, Qt::CaseInsensitive ) == 0  &&
                 model.run_of_row( row ) >= 0 )
               return row;
         return -1;
      }

      static QTreeWidgetItem* itemOfRow( QTreeWidget& tw, int row )
      {
         QList< QTreeWidgetItem* > stack;

         for ( int ii = 0; ii < tw.topLevelItemCount(); ii++ )
            stack << tw.topLevelItem( ii );

         while ( ! stack.isEmpty() )
         {
            QTreeWidgetItem* item = stack.takeFirst();

            if ( item->type() - (int)QTreeWidgetItem::UserType == row )
               return item;

            for ( int ii = 0; ii < item->childCount(); ii++ )
               stack << item->child( ii );
         }

         return nullptr;
      }
};

/* An edit with a model and noise of the investigator's, and noise of
   another investigator's model of another edit recorded against it.

   An older delete_editedData removes the edit's model and noise, then fails
   on that noise (fk_noise_editDataID), keeping what it removed unless the
   caller's transaction gives it back; a patched one removes that noise too,
   all or nothing.  Either way the database never loses part of the edit,
   and the rows say what it holds.
*/
TEST_F( TestUSDataDbLive, AnEditRemovalLosesNothingItDoesNotReport )
{
   QString stamp = QDateTime::currentDateTimeUtc().toString( "yyMMddhhmm" );
   QString editG = US_Util::new_guid();

   int rawID = -1;
   QString error;
   ASSERT_EQ( US_ConvertIO::findRawDataId( inv, rawGUID, rawID, error ), IUS_DB2::OK )
      << error.toStdString();

   US_ConvertIO::EditedDataRecord record;
   record.rawDataID = rawID;
   record.editGUID  = editG;
   record.label     = runID;
   record.filename  = runID + "." + stamp + ".RA.1.A.280.xml";
   record.comment   = "live removal test";

   int editID = -1;
   ASSERT_EQ( US_ConvertIO::createEditedData( inv, record, editID, error ), IUS_DB2::OK )
      << error.toStdString();
   cleanup << "delete_editedData " + QString::number( editID );

   QString base   = runID + ".1A280.e" + stamp;
   QString modelG = US_Util::new_guid();
   QString noiseG = US_Util::new_guid();
   QString theirM = US_Util::new_guid();
   QString theirN = US_Util::new_guid();

   int modelID = newModel( inv, modelG, editG, base + "_a1_2DSA_i01.model", personID );
   int noiseID = newNoise( inv, noiseG, editID, modelG, base + "_a1_2DSA_i01.ti_noise" );
   int othM    = newModel( admin, theirM, othEdit, "Other_run.1A280.global.model", otherID );
   int othN    = newNoise( admin, theirN, editID, theirM, "Other_run.1A280.global.ti_noise" );
   ASSERT_GT( modelID, 0 );
   ASSERT_GT( noiseID, 0 );
   ASSERT_GT( othM, 0 );
   ASSERT_GT( othN, 0 );
   cleanup.prepend( "delete_model " + QString::number( othM ) );
   cleanup.prepend( "delete_noise " + QString::number( othN ) );

   // The investigator's view of the run, from the database
   QProgressBar bar;
   QLabel       label;
   US_DataModel model;
   model.setDatabase( inv );
   model.setProgress( &bar, &label );
   model.setFilters( runID, "ALL", "DB Only" );
   model.scan_data();

   US_DataProcess proc( &model );

   auto rowOf = [ & ]( const QString& guid )
   {
      for ( int row = 0; row < model.recCount(); row++ )
         if ( model.row_datadesc( row ).dataGUID.compare( guid, Qt::CaseInsensitive ) == 0  &&
              model.run_of_row( row ) >= 0 )
            return row;
      return -1;
   };

   int erow = rowOf( editG );
   ASSERT_GE( erow, 0 ) << model.scan_errors().join( "\n" ).toStdString();
   ASSERT_GE( rowOf( modelG ), 0 );
   ASSERT_GE( rowOf( noiseG ), 0 );
   EXPECT_EQ( rowOf( theirM ), -1 );          // not the investigator's

   int stat = proc.record_remove_db( erow );

   int hasEdit  = present( "get_editedData", editID,  IUS_DB2::NO_EDITDATA );
   int hasModel = present( "get_model_info", modelID, IUS_DB2::NO_MODEL );
   int hasNoise = present( "get_noise_info", noiseID, IUS_DB2::NO_NOISE );
   int hasTheir = present( "get_noise_info", othN,    IUS_DB2::NO_NOISE );

   if ( stat == 0 )
   {  // a delete_editedData that removes the noise recorded against the edit
      EXPECT_EQ( hasEdit,  0 );
      EXPECT_EQ( hasModel, 0 );
      EXPECT_EQ( hasNoise, 0 );
      EXPECT_EQ( hasTheir, 0 );
   }

   else
   {  // the older one: refused, and nothing is lost
      EXPECT_EQ( stat, 2012 ) << proc.lastError().toStdString();
      EXPECT_TRUE( proc.lastError().contains( "fk_noise_editDataID" ) )
         << proc.lastError().toStdString();
      EXPECT_TRUE( proc.lastError().contains( "Nothing was removed" ) )
         << proc.lastError().toStdString();
      EXPECT_EQ( hasEdit,  1 );
      EXPECT_EQ( hasModel, 1 );
      EXPECT_EQ( hasNoise, 1 );
      EXPECT_EQ( hasTheir, 1 );
   }

   // Every row says what the database holds
   for ( const QString& guid : { editG, modelG, noiseG } )
   {
      int  has   = ( guid == editG ) ? hasEdit : ( guid == modelG ) ? hasModel : hasNoise;
      bool inRow = ( model.row_datadesc( rowOf( guid ) ).recState
                     & US_DataModel::REC_DB ) != 0;
      EXPECT_EQ( inRow, has == 1 ) << guid.toStdString();
   }

   // The other investigator's noise goes first now, and the edit with it
   if ( stat != 0 )
   {
      EXPECT_EQ( admin->statusQuery( QStringList() << "delete_noise"
                                     << QString::number( othN ) ), IUS_DB2::OK );
      EXPECT_EQ( proc.record_remove_db( rowOf( editG ) ), 0 )
         << proc.lastError().toStdString();
      EXPECT_EQ( present( "get_editedData", editID,  IUS_DB2::NO_EDITDATA ), 0 );
      EXPECT_EQ( present( "get_model_info", modelID, IUS_DB2::NO_MODEL ), 0 );
   }
}

/* The same edit, removed through the tree as the window does it: the
   selection of the edit takes its model and noise along.  Whatever the
   procedure, nothing under an edit the database keeps is removed one by one
   instead, nothing is lost that the message does not name, and every row
   says what the database holds.
*/
TEST_F( TestUSDataDbLive, AnEditTheDatabaseKeepsKeepsItsSubtree )
{
   Edit ed;
   newEdit( ed );
   if ( HasFatalFailure() )   return;

   QProgressBar bar;
   QLabel       label;
   US_DataModel model;
   model.setDatabase( inv );
   model.setProgress( &bar, &label );
   model.setFilters( runID, "ALL", "DB Only" );

   QTreeWidget    tw;
   US_DataProcess proc( &model );
   US_DataTree    tree( &model, &tw );
   QStringList    prompts;
   tw.setSelectionMode( QAbstractItemView::ExtendedSelection );
   model.setSiblings( &proc, &tree );
   tree.setPrompter( [ & ]( int, const QString& title, const QString& text )
                     { prompts << title + ": " + text; return true; } );

   model.scan_data();
   tree.build_dtree();

   int erow = rowOf( model, ed.editG );
   ASSERT_GE( erow, 0 ) << model.scan_errors().join( "\n" ).toStdString();
   ASSERT_GE( rowOf( model, ed.modelG ), 0 );
   ASSERT_GE( rowOf( model, ed.noiseG ), 0 );

   QTreeWidgetItem* item = itemOfRow( tw, erow );
   ASSERT_NE( item, nullptr );
   tw.clearSelection();
   item->setSelected( true );
   ASSERT_EQ( tree.action_row_list().size(), 3 );

   int stat = tree.act_on_selection( "Database only", "DB-only remove" );
   QString error = proc.lastError();

   int hasEdit  = present( "get_editedData", ed.editID,  IUS_DB2::NO_EDITDATA );
   int hasModel = present( "get_model_info", ed.modelID, IUS_DB2::NO_MODEL );
   int hasNoise = present( "get_noise_info", ed.noiseID, IUS_DB2::NO_NOISE );
   int hasTheir = present( "get_noise_info", ed.othN,    IUS_DB2::NO_NOISE );

   EXPECT_TRUE( prompts.isEmpty() ) << prompts.join( "\n" ).toStdString();

   // The other investigator's noise is never removed without a word: the
   // database keeps the edit, and all of it
   EXPECT_NE( stat, 0 ) << error.toStdString();
   EXPECT_EQ( hasEdit,  1 );
   EXPECT_EQ( hasModel, 1 ) << error.toStdString();
   EXPECT_EQ( hasNoise, 1 ) << error.toStdString();
   EXPECT_EQ( hasTheir, 1 );
   EXPECT_TRUE( error.contains( "not removed either" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "None of the selected records was removed" ) )
      << error.toStdString();

   for ( const QString& guid : { ed.editG, ed.modelG, ed.noiseG } )
      EXPECT_NE( model.row_datadesc( rowOf( model, guid ) ).recState
                 & US_DataModel::REC_DB, 0 ) << guid.toStdString();
}
