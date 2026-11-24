// test/programs/us_data_publication/datapub_fake_db.h
#pragma once

#include <gmock/gmock.h>

#include <QtCore>

#include "mock_us_db2.h"
#include "us_util.h"

/**
 * A database for the import tests, on the IUS_DB2 gmock mock.
 *
 * A full import runs a few hundred procedure calls, so scripting them one
 * expectation at a time would test the script rather than the importer.
 * This fake answers them instead: every "new_..." call registers the first
 * GUID it is given under the record family of the procedure, owned by the
 * person the connection is logged in as.  The log of calls says what
 * happened in which order.
 *
 * Lookups and listings are scoped as the server scopes them (us3_sql):
 * - get_bufferID, get_analyteID, get_editID, get_modelID, get_noiseID,
 *   get_personID_from_GUID and get_rotorID_from_GUID find a GUID whoever
 *   owns the record: those procedures check no permission at all.
 * - get_projectID_from_GUID, get_solutionID_from_GUID and
 *   get_rawDataID_from_GUID check the logged-in person's permission
 *   (verify_project/solution/experiment_permission): someone else's record
 *   answers NOTPERMITTED, unless the person is an admin.
 * - The listings get_project_desc, get_buffer_desc, get_analyte_desc,
 *   all_solutionIDs, all_rawDataIDs, all_editedDataIDs, get_model_desc and
 *   get_noise_desc
 *   list the records of the investigator they are given; asking for
 *   someone else's answers NOTPERMITTED unless the person is an admin.  A
 *   listing a test canned wins.  The owner of raw data and of an edit
 *   stands in for the experimentPerson row of its run.
 * - get_experiment_desc and get_experiment_info_by_runID are an
 *   investigator's; get_experiment_info answers for the person's own runs,
 *   or for any run when the person is an admin.
 * - An experiment GUID is unique (experiment.experimentGUID is UNIQUE in
 *   us3.sql): a second new_experiment of the same GUID fails.
 * - get_user_info answers with the person's user level: 3 for an admin,
 *   userLevel (2, as a plain investigator has) otherwise.
 * - add_rotor_calibration takes an admin (NOTPERMITTED otherwise) and an
 *   experiment that exists, or -1 for none (NO_EXPERIMENT otherwise: 0 is
 *   an ID it looks for).
 * - new_solutionAnalyte refuses an analyte that is not there (NO_ANALYTE)
 *   and, unless the person is an admin, one that is not theirs
 *   (verify_analyte_permission: NOTPERMITTED).
 * - get_buffer_info answers for the person's own buffers, public ones and
 *   anyone's for an admin, with the owner and the private flag in columns
 *   7 and 8; someone else's private buffer is NOTPERMITTED.  A buffer the
 *   import writes is private, as new_buffer is told.
 * - The raw data and solution rows name channel 1 (fk_rawData_channelID,
 *   fk_experimentSolutionChannel_channelID): without channelRow1 they fail,
 *   and the import's check of the channel table answers NOROWS.
 * - get_operator_names lists the operators with a permit for the
 *   instrument (the person logged in, once addHardware() ran), and
 *   new_experiment refuses any other operator (BADOPERATOR).
 * - new_noise of an edit that is not there falls back on edit 1, which
 *   this database does not have: the write fails.
 * - create_buffer_component takes level 2 and inserts with the ID of the
 *   row count plus one -- failing when that ID is taken -- and leaves the
 *   connection's last insert ID as it was, as the unpatched procedure does
 *   (LAST_INSERT_ID() is not set by an explicit ID).  The components are
 *   listed and read (get_buffer_component_desc/_info) from the table,
 *   unless a test canned those listings.
 *
 * Transactions behave as InnoDB's do: what is written inside one is gone
 * after a rollback, and also when the connection is dropped without a
 * commit (dropConnection()).  Writes outside a transaction are kept at
 * once.  A procedure named in implicitCommitOn commits implicitly, as a
 * COMMIT inside a procedure or DDL does: what was written up to it is kept,
 * the server's transaction is over, later writes are kept as they are made,
 * and the commit or rollback that follows fails as US_DB2's does when the
 * server no longer has the transaction open.  Blobs keep their bytes, so a
 * test can look at what was uploaded.
 *
 * A failure can be injected for one procedure (or blob procedure); the call
 * then reports DBERROR, as US_DB2 does for any MySQL error.
 *
 * Text goes into columns and procedure parameters of the character set
 * `charset` (utf8mb4 unless a test says otherwise), which information_schema
 * and @@character_set_database report (raw queries).  With latin1, a write
 * whose text holds a character beyond U+00FF is refused with DBERROR, as a
 * strict server refuses it ("Incorrect string value", 1366).
 */
class DataPubFakeDb
{
   public:
      ::testing::NiceMock< US_DB2_Mock > mock;

      QStringList log;           // Every procedure called, in order
      QList< QStringList > calls;// Every call with its arguments
      int  begins    = 0;
      int  commits   = 0;
      int  rollbacks = 0;
      int  beginAt   = -1;       // The log size when the transaction began
      int  commitAt  = -1;       // The log size when the commit came
      bool inTxn     = false;    // The client's view: begun and not ended
      bool serverTxn = false;    // The server's view: still open

      int  person    = 2;        // The person the connection is logged in as
      bool admin     = false;    // That person is an admin
      int  userLevel = 2;        // Their user level, when not an admin

      // The people with a permit for the instrument (get_operator_names);
      // new_experiment refuses any other operator (BADOPERATOR)
      QList< int > operators;
      bool channelRow1 = true;   // The channel table has channel 1

      QString charset = "utf8mb4"; // Of the target's text columns and parameters
      bool    infoSchema = true; // information_schema shows the charsets

      QString failOn;            // A procedure that fails with DBERROR
      QString implicitCommitOn;  // A procedure that commits implicitly

      // Called as a transaction begins, to change the database between an
      // import's preflight and its writes
      std::function< void() > onBegin;

      // The stored state: what a commit keeps and a rollback restores
      class State
      {
         public:
            // family -> GUID -> ID, and family -> GUID -> owner
            QMap< QString, QMap< QString, int > > registry;
            QMap< QString, QMap< QString, int > > owners;

            // family -> GUID -> the name it was written with, and a model's
            // edit GUID
            QMap< QString, QMap< QString, QString > > names;
            QMap< QString, QString >                  modelEdits;

            // experiment ID -> its row; time state by experiment ID
            class Experiment
            {
               public:
                  QString guid;
                  QString runID;
                  int     owner = 2;
            };

            QMap< int, Experiment > experiments;
            QMap< int, int >        timestates;

            // Buffers that are public (private = 0), by GUID; any other is
            // private
            QSet< QString >         publicBuffers;

            // The buffer component table: ID -> the row of
            // get_buffer_component_info (unit, name, viscosity, density,
            // range, gradient forming)
            QMap< int, QVariantList > components;

            // "procedure:id" -> the bytes uploaded
            QMap< QString, QByteArray > blobs;
      };

      State state;

      // The registry of the stored state, as the tests used it before
      QMap< QString, QMap< QString, int > >& registry = state.registry;

      // procedure -> rows, for the listings a test sets up
      QMap< QString, QList< QVariantList > > canned;

      // "procedure:first argument" -> rows, which win over canned
      QMap< QString, QList< QVariantList > > cannedFor;

      DataPubFakeDb()
      {
         using ::testing::_;
         using ::testing::An;
         using ::testing::Invoke;

         ON_CALL( mock, isConnected() ).WillByDefault( ::testing::Return( true ) );
         ON_CALL( mock, query( An< const QStringList& >() ) )
            .WillByDefault( Invoke( [ this ]( const QStringList& q )
                                    { handle( q ); } ) );
         ON_CALL( mock, statusQuery( An< const QStringList& >() ) )
            .WillByDefault( Invoke( [ this ]( const QStringList& q )
                                    { handle( q ); return status; } ) );
         ON_CALL( mock, statusQuery( An< const QString& >() ) )
            .WillByDefault( Invoke( [ this ]( const QString& sql )
                                    { return handleSql( sql ); } ) );
         ON_CALL( mock, rawQuery( _ ) )
            .WillByDefault( Invoke( [ this ]( const QString& sql )
                                    { handleSql( sql ); } ) );
         ON_CALL( mock, next() )
            .WillByDefault( Invoke( [ this ]()
                                    { return ++cursor <= rows.size(); } ) );
         ON_CALL( mock, value( _ ) )
            .WillByDefault( Invoke( [ this ]( unsigned ii ) -> QVariant
            {
               if ( cursor < 1  ||  cursor > rows.size() )  return QVariant();
               const QVariantList& row = rows[ cursor - 1 ];
               return int( ii ) < row.size() ? row[ ii ] : QVariant();
            } ) );
         ON_CALL( mock, lastErrno() )
            .WillByDefault( Invoke( [ this ]() { return status; } ) );
         ON_CALL( mock, lastError() )
            .WillByDefault( Invoke( [ this ]()
            { return status == IUS_DB2::OK ? QString()
                                           : QString( "fake error" ); } ) );
         ON_CALL( mock, lastInsertID() )
            .WillByDefault( Invoke( [ this ]() { return insertID; } ) );
         ON_CALL( mock, writeBlobToDB( _, _, _ ) )
            .WillByDefault( Invoke( [ this ]( const QString& file,
                                              const QString& proc, int id )
            {
               log   << proc;
               calls << ( QStringList() << proc << file
                                        << QString::number( id ) );
               QFile blob( file );

               if ( proc == failOn  ||  ! blob.open( QIODevice::ReadOnly ) )
               {
                  status = IUS_DB2::DBERROR;
                  return status;
               }

               state.blobs.insert( proc + ":" + QString::number( id ),
                                   blob.readAll() );
               blob.close();
               status = IUS_DB2::OK;
               return status;
            } ) );
         ON_CALL( mock, transactionActive() )
            .WillByDefault( Invoke( [ this ]() { return inTxn; } ) );
         ON_CALL( mock, beginTransaction( _ ) )
            .WillByDefault( Invoke( [ this ]( QString& error )
            {
               error.clear();
               if ( inTxn )  return false;
               if ( onBegin )  onBegin();
               begins++;
               beginAt   = log.size();
               saved     = state;
               inTxn     = true;
               serverTxn = true;
               return true;
            } ) );
         ON_CALL( mock, commitTransaction( _ ) )
            .WillByDefault( Invoke( [ this ]( QString& error )
            {
               error.clear();
               if ( ! inTxn )  return false;

               // As US_DB2 does: a COMMIT after the server ended the
               // transaction commits nothing, so it is refused, and the
               // transaction still counts as open to the client
               if ( ! serverTxn )
               {
                  error = endedText();
                  return false;
               }

               commits++;
               commitAt  = log.size();
               inTxn     = false;
               serverTxn = false;
               return true;
            } ) );
         ON_CALL( mock, rollbackTransaction( _ ) )
            .WillByDefault( Invoke( [ this ]( QString& error )
            {
               error.clear();
               if ( ! inTxn )  return false;
               rollbacks++;
               inTxn = false;

               // Once the server ended it there is nothing to undo: what it
               // kept stays, and the rollback says so
               if ( ! serverTxn )
               {
                  error = endedText();
                  return false;
               }

               state     = saved;
               serverTxn = false;
               return true;
            } ) );
      }

      IUS_DB2* db() { return &mock; }

      // The connection goes away: an open transaction is rolled back by the
      // server, never committed
      void dropConnection()
      {
         if ( ! inTxn )  return;

         if ( serverTxn )  state = saved;

         inTxn     = false;
         serverTxn = false;
      }

      // What the commit and rollback say once the server ended the
      // transaction, in the words of US_DB2's message
      static QString endedText()
      {
         return QString( "The transaction had already ended on the server;"
                         " some rows may have been kept" );
      }

      // An experiment already in the database, of the given owner
      void addExperiment( int id, const QString& guid, const QString& runID,
                          int owner = 2 )
      {
         State::Experiment exp;
         exp.guid  = guid;
         exp.runID = runID;
         exp.owner = owner;
         state.experiments.insert( id, exp );
         state.registry[ "experiment" ].insert( guid, id );
         state.owners  [ "experiment" ].insert( guid, owner );
      }

      // A record already in the database, of the given owner
      void addRecord( const QString& family, const QString& guid, int id,
                      int owner = 2, const QString& name = QString() )
      {
         state.registry[ family ].insert( guid, id );
         state.owners  [ family ].insert( guid, owner );

         if ( ! name.isEmpty() )  state.names[ family ].insert( guid, name );
      }

      // A buffer component in the table, under the given ID
      void addComponent( int id, const QString& name, const QString& unit,
                         const QString& range, const QString& density,
                         const QString& viscosity )
      {
         state.components.insert( id, QVariantList() << unit << name
                                      << viscosity << density << range
                                      << "0" );
      }

      // The calls of one procedure
      QList< QStringList > callsOf( const QString& proc ) const
      {
         QList< QStringList > found;

         for ( int ii = 0; ii < calls.size(); ii++ )
            if ( calls[ ii ][ 0 ] == proc )  found << calls[ ii ];

         return found;
      }

      // How many calls of a procedure came after the transaction began
      int callsInTransaction( const QString& proc ) const
      {
         int knt = 0;

         for ( int ii = qMax( 0, beginAt ); ii < log.size(); ii++ )
            if ( beginAt >= 0  &&  log[ ii ] == proc )  knt++;

         return knt;
      }

      // How many writes ("new_", "add_", "update_", "delete_", blobs) ran
      int writes() const
      {
         int knt = 0;

         for ( int ii = 0; ii < log.size(); ii++ )
            if ( isWrite( log[ ii ] ) )  knt++;

         return knt;
      }

      // How many writes ran before the first transaction began -- or at
      // all, when none began
      int writesBeforeBegin() const
      {
         int end = ( beginAt < 0 ) ? log.size() : beginAt;
         int knt = 0;

         for ( int ii = 0; ii < end; ii++ )
            if ( isWrite( log[ ii ] ) )  knt++;

         return knt;
      }

      // The procedures called inside the last transaction, in order
      QStringList transactionLog() const
      {
         if ( beginAt < 0 )  return QStringList();

         int end = ( commitAt < beginAt ) ? log.size() : commitAt;

         return log.mid( beginAt, end - beginAt );
      }

      static bool isWrite( const QString& proc )
      {
         return proc.startsWith( "new_" )    ||  proc.startsWith( "add_" )  ||
                proc.startsWith( "create_" ) ||  proc.startsWith( "set_" )  ||
                proc.startsWith( "update_" ) ||  proc.startsWith( "delete_" ) ||
                proc.startsWith( "upload_" );
      }

      // A lab with one instrument of the given serial, one rotor with one
      // calibration, and one centerpiece
      void addHardware( const QString& instrumentSerial )
      {
         QVariantList inst;
         inst << "Instrument" << instrumentSerial;
         while ( inst.size() < 16 )  inst << QVariant( "" );

         QVariantList rotor;
         rotor << targetRotorGUID << "Target rotor" << "R-1" << "" << 1
               << US_Util::new_guid();

         QVariantList cal;
         cal << targetCalGUID << 3 << targetRotorGUID << "report" << 1.0e-5
             << 2.0e-10 << 0.0 << "2024-01-01 00:00:00" << 0 << "Target cal";

         QVariantList cp;
         cp << targetCpGUID << "Target centerpiece" << 2 << "7.2" << "standard"
            << 60000 << 1.2 << 2.5 << 0.5;
         while ( cp.size() < 12 )  cp << QVariant( "" );

         canned.insert( "get_lab_names", QList< QVariantList >()
                        << ( QVariantList() << 1 ) );
         canned.insert( "get_lab_info", QList< QVariantList >()
                        << ( QVariantList() << US_Util::new_guid() << "Lab"
                                            << "B" << "R" ) );
         canned.insert( "get_instrument_names", QList< QVariantList >()
                        << ( QVariantList() << 5 ) );
         canned.insert( "get_instrument_info_new", QList< QVariantList >()
                        << inst );
         operators << person;
         canned.insert( "get_rotor_names", QList< QVariantList >()
                        << ( QVariantList() << 3 ) );
         canned.insert( "get_rotor_info", QList< QVariantList >() << rotor );
         canned.insert( "get_rotor_calibration_profiles",
                        QList< QVariantList >() << ( QVariantList() << 9 ) );
         canned.insert( "get_rotor_calibration_info",
                        QList< QVariantList >() << cal );
         canned.insert( "get_abstractCenterpiece_names",
                        QList< QVariantList >() << ( QVariantList() << 7 ) );
         canned.insert( "get_abstractCenterpiece_info",
                        QList< QVariantList >() << cp );
      }

      QString targetRotorGUID = US_Util::new_guid();
      QString targetCalGUID   = US_Util::new_guid();
      QString targetCpGUID    = US_Util::new_guid();

   private:
      State                 saved;
      QList< QVariantList > rows;
      int                   cursor   = 0;
      int                   status   = IUS_DB2::OK;
      int                   insertID = 0;
      int                   nextID   = 100;

      static QString familyOfLookup( const QString& proc )
      {
         static QMap< QString, QString > families;

         if ( families.isEmpty() )
         {
            families.insert( "get_projectID_from_GUID",  "project"  );
            families.insert( "get_bufferID",             "buffer"   );
            families.insert( "get_analyteID",            "analyte"  );
            families.insert( "get_solutionID_from_GUID", "solution" );
            families.insert( "get_rawDataID_from_GUID",  "rawData"  );
            families.insert( "get_editID",               "edit"     );
            families.insert( "get_modelID",              "model"    );
            families.insert( "get_noiseID",              "noise"    );
            families.insert( "get_personID_from_GUID",   "person"   );
            families.insert( "get_rotorID_from_GUID",    "rotor"    );
         }

         return families.value( proc );
      }

      // The lookups that check the logged-in person's permission
      static bool scopedLookup( const QString& proc )
      {
         return proc == "get_projectID_from_GUID"   ||
                proc == "get_solutionID_from_GUID"  ||
                proc == "get_rawDataID_from_GUID";
      }

      // The argument a write gives its record's name in
      static int nameArgOf( const QString& proc )
      {
         if ( proc == "new_buffer"   )  return 2;
         if ( proc == "new_analyte"  )  return 5;
         if ( proc == "new_solution" )  return 2;
         if ( proc == "new_model"    )  return 2;
         if ( proc == "new_noise"    )  return 6;
         if ( proc == "new_project2" )  return 11;
         return 0;
      }

      // The family an investigator's listing lists
      static QString listingFamily( const QString& proc )
      {
         if ( proc == "get_project_desc"  )  return "project";
         if ( proc == "get_buffer_desc"   )  return "buffer";
         if ( proc == "get_analyte_desc"  )  return "analyte";
         if ( proc == "all_solutionIDs"   )  return "solution";
         if ( proc == "all_editedDataIDs" )  return "edit";
         if ( proc == "all_rawDataIDs"    )  return "rawData";
         if ( proc == "get_model_desc"    )  return "model";
         if ( proc == "get_noise_desc"    )  return "noise";
         return QString();
      }

      // The rows of a listing, in the columns of its procedure
      void listRecords( const QString& proc, int inv )
      {
         QString              family = listingFamily( proc );
         QMap< QString, int > ids    = state.registry.value( family );
         QList< QString >     guids  = ids.keys();

         for ( int ii = 0; ii < guids.size(); ii++ )
         {
            QString guid  = guids[ ii ];
            int     owner = state.owners.value( family ).value( guid, person );

            if ( inv != 0  &&  owner != inv )  continue;

            int          id   = ids.value( guid );
            QString      name = state.names.value( family ).value( guid );
            QVariantList row;

            if ( proc == "get_model_desc" )
            {
               QString edit = state.modelEdits.value( guid );
               row << id << guid << name << 0.0 << 0.0 << edit
                   << state.registry.value( "edit" ).value( edit );
            }

            else if ( proc == "get_noise_desc" )
               row << id << guid << 0 << 0 << "ti" << "" << "" << "" << ""
                   << name;

            else if ( proc == "all_editedDataIDs"  ||
                      proc == "all_rawDataIDs" )
               row << id << name << name << 0 << 0 << "" << "" << 0 << ""
                   << guid;

            else
               row << id << name;

            rows << row;
         }
      }

      static QString familyOfWrite( const QString& proc )
      {
         static QMap< QString, QString > families;

         if ( families.isEmpty() )
         {
            families.insert( "new_project2",          "project"     );
            families.insert( "new_buffer",            "buffer"      );
            families.insert( "new_analyte",           "analyte"     );
            families.insert( "new_solution",          "solution"    );
            families.insert( "new_experiment",        "experiment"  );
            families.insert( "new_rawData",           "rawData"     );
            families.insert( "new_editedData",        "edit"        );
            families.insert( "new_model",             "model"       );
            families.insert( "new_noise",             "noise"       );
            families.insert( "add_rotor_calibration", "calibration" );
         }

         return families.value( proc );
      }

      static bool isGuid( const QString& text )
      {
         return US_Util::UUID_REGEX.match( text ).hasMatch();
      }

      int level() const
      {
         return admin ? 3 : userLevel;
      }

      // A raw SQL statement: only the import's look at the channel table
      int handleSql( const QString& sql )
      {
         log   << "sql";
         calls << ( QStringList() << "sql" << sql );
         rows.clear();
         cursor = 0;

         if ( failOn == "sql" )  return status = IUS_DB2::DBERROR;

         if ( sql.contains( "FROM channel" ) )
            status = channelRow1 ? int( IUS_DB2::OK )
                                 : int( IUS_DB2::NOROWS );

         // The character sets of the procedures and tables asked about
         else if ( sql.contains( "information_schema" ) )
         {
            static const QRegularExpression quoted( "'([A-Za-z_0-9]+)'" );
            int split = sql.indexOf( "UNION" );
            QRegularExpressionMatchIterator it = quoted.globalMatch( sql );

            while ( infoSchema  &&  it.hasNext() )
            {
               QRegularExpressionMatch match = it.next();
               QString name = match.captured( 1 );

               if ( name == "p"  ||  name == "t" )  continue;

               rows << ( QVariantList()
                         << ( match.capturedStart() < split ? "p" : "t" )
                         << name.toLower() << charset );
            }

            status = IUS_DB2::OK;
         }

         else if ( sql.contains( "@@character_set_database" ) )
         {
            rows << ( QVariantList() << charset );
            status = IUS_DB2::OK;
         }
         else
            status = IUS_DB2::DBERROR;

         return status;
      }

      // What the server's own checks refuse among the writes; true when the
      // write was refused, with the status set
      bool refuses( const QString& proc, const QStringList& q )
      {
         // Text a latin1 column or parameter cannot hold
         if ( charset == "latin1" )
         {
            for ( int ii = 1; ii < q.size(); ii++ )
               for ( const QChar& ch : q[ ii ] )
                  if ( ch.unicode() > 0xFF )
                  {
                     status = IUS_DB2::DBERROR;
                     return true;
                  }
         }

         if ( proc == "add_rotor_calibration" )
         {
            if ( level() < 3 )
            {
               status = IUS_DB2::NOTPERMITTED;
               return true;
            }

            int expID = q.value( 7 ).toInt();

            if ( expID != -1  &&  ! state.experiments.contains( expID ) )
            {
               status = IUS_DB2::NO_EXPERIMENT;
               return true;
            }
         }

         if ( proc == "new_experiment"  &&
              ! operators.contains( q.value( 6 ).toInt() ) )
         {
            status = IUS_DB2::BADOPERATOR;
            return true;
         }

         if ( proc == "new_solutionAnalyte" )
         {
            QString guid = q.value( 3 );

            if ( ! state.registry.value( "analyte" ).contains( guid ) )
            {
               status = IUS_DB2::NO_ANALYTE;
               return true;
            }

            if ( ! admin  &&
                 state.owners.value( "analyte" ).value( guid, person )
                 != person )
            {
               status = IUS_DB2::NOTPERMITTED;
               return true;
            }
         }

         // An edit the person may not use (or that is not there) makes
         // new_noise fall back on edit 1, the "unassigned" record, which a
         // database that does not have it refuses on its foreign key
         if ( proc == "new_noise" )
         {
            int  editID = q.value( 2 ).toInt();
            bool known  = false;

            for ( int id : state.registry.value( "edit" ) )
               if ( id == editID )  known = true;

            if ( ! known )
            {
               status = IUS_DB2::DBERROR;
               return true;
            }
         }

         if ( ! channelRow1  &&
              ( proc == "new_rawData"  ||  proc == "new_solution"  ||
                proc == "new_experiment_solution" ) )
         {
            status = IUS_DB2::DBERROR;      // A foreign key constraint fails
            return true;
         }

         return false;
      }

      // create_buffer_component, as the unpatched procedure does it
      void createComponent( const QStringList& q )
      {
         if ( level() < 2 )
         {
            status = IUS_DB2::NOTPERMITTED;
            return;
         }

         int id = state.components.size() + 1;

         if ( state.components.contains( id ) )
         {
            status = IUS_DB2::DBERROR;      // Duplicate entry for the key
            return;
         }

         // q: proc, componentGUID, name, unit, range, grad_form, density,
         // viscosity; insertID is left as the last insert made it
         state.components.insert( id, QVariantList() << q.value( 3 )
                                      << q.value( 2 ) << q.value( 7 )
                                      << q.value( 6 ) << q.value( 4 )
                                      << q.value( 5 ) );
         status = IUS_DB2::OK;
      }

      void handle( const QStringList& q )
      {
         QString proc = q.value( 0 );
         log   << proc;
         calls << q;
         rows.clear();
         cursor = 0;

         if ( proc == failOn )
         {
            status = IUS_DB2::DBERROR;
            return;
         }

         if ( proc == "get_user_info" )
         {
            rows << ( QVariantList() << person << "Test" << "Person" << ""
                                     << "" << level() );
            status = IUS_DB2::OK;
            return;
         }

         if ( proc == "create_buffer_component" )
         {
            createComponent( q );
            return;
         }

         if ( proc == "get_operator_names"  &&  ! canned.contains( proc ) )
         {
            for ( int ii = 0; ii < operators.size(); ii++ )
               rows << ( QVariantList() << operators[ ii ]
                         << QString( "aaaaaaaa-0000-4000-8000-%1" )
                            .arg( operators[ ii ], 12, 10, QChar( '0' ) )
                         << "Operator" << QString::number( operators[ ii ] ) );

            status = rows.isEmpty() ? int( IUS_DB2::NOROWS )
                                    : int( IUS_DB2::OK );
            return;
         }

         if ( proc == "get_buffer_component_desc"  &&
              ! canned.contains( proc )  &&
              ! cannedFor.contains( proc + ":" + q.value( 1 ) ) )
         {
            QList< int > ids = state.components.keys();

            for ( int ii = 0; ii < ids.size(); ii++ )
               rows << ( QVariantList() << ids[ ii ]
                                        << state.components[ ids[ ii ] ]
                                           .value( 1 ) );

            status = rows.isEmpty() ? int( IUS_DB2::NOROWS )
                                    : int( IUS_DB2::OK );
            return;
         }

         if ( proc == "get_buffer_component_info"  &&
              ! canned.contains( proc )  &&
              ! cannedFor.contains( proc + ":" + q.value( 1 ) ) )
         {
            int id = q.value( 1 ).toInt();

            if ( state.components.contains( id ) )
               rows << state.components.value( id );

            status = rows.isEmpty() ? int( IUS_DB2::NOROWS )
                                    : int( IUS_DB2::OK );
            return;
         }

         if ( proc == "get_buffer_info"  &&  ! canned.contains( proc )  &&
              ! cannedFor.contains( proc + ":" + q.value( 1 ) ) )
         {
            QMap< QString, int > ids = state.registry.value( "buffer" );
            QString              guid;

            for ( auto it = ids.constBegin(); it != ids.constEnd(); ++it )
               if ( QString::number( it.value() ) == q.value( 1 ) )
                  guid = it.key();

            if ( guid.isEmpty() )
            {
               status = IUS_DB2::NOROWS;
               return;
            }

            int  owner = state.owners.value( "buffer" ).value( guid, person );
            bool priv  = ! state.publicBuffers.contains( guid );

            if ( ! admin  &&  owner != person  &&  priv )
            {
               status = IUS_DB2::NOTPERMITTED;
               return;
            }

            rows << ( QVariantList() << guid
                      << state.names.value( "buffer" ).value( guid )
                      << "0" << "7" << "1.002" << "1.0" << "0" << owner
                      << ( priv ? "1" : "0" ) );
            status = IUS_DB2::OK;
            return;
         }

         QString family = familyOfLookup( proc );

         if ( ! family.isEmpty() )
         {
            QString guid = q.value( 1 );

            if ( state.registry.value( family ).contains( guid ) )
            {
               // The permission-checked lookups refuse someone else's
               if ( scopedLookup( proc )  &&  ! admin  &&
                    state.owners.value( family ).value( guid, person )
                    != person )
               {
                  status = IUS_DB2::NOTPERMITTED;
                  return;
               }

               rows << ( QVariantList() << state.registry[ family ][ guid ] );
               status = IUS_DB2::OK;
            }
            else
               status = IUS_DB2::NOROWS;

            return;
         }

         if ( isWrite( proc ) )
         {
            family   = familyOfWrite( proc );

            if ( refuses( proc, q ) )  return;

            // An experiment GUID is unique
            if ( proc == "new_experiment"  &&
                 state.registry.value( "experiment" ).contains( q.value( 1 ) ) )
            {
               status = IUS_DB2::DBERROR;
               return;
            }

            insertID = nextID++;
            status   = IUS_DB2::OK;

            for ( int ii = 1; ii < q.size()  &&  ! family.isEmpty(); ii++ )
            {
               if ( ! isGuid( q[ ii ] ) )  continue;

               state.registry[ family ].insert( q[ ii ], insertID );
               state.owners  [ family ].insert( q[ ii ], person );

               if ( nameArgOf( proc ) > 0 )
                  state.names[ family ].insert( q[ ii ],
                                                q.value( nameArgOf( proc ) ) );

               if ( proc == "new_model" )
                  state.modelEdits.insert( q[ ii ], q.value( 6 ) );

               break;
            }

            // What was written up to here is kept, and the rest runs
            // outside any transaction
            if ( proc == implicitCommitOn  &&  serverTxn )
               serverTxn = false;

            if ( proc == "new_experiment" )
            {
               State::Experiment exp;
               exp.guid  = q.value( 1 );
               exp.runID = q.value( 3 );
               exp.owner = person;
               state.experiments.insert( insertID, exp );
            }

            if ( proc == "new_timestate" )
               state.timestates.insert( q.value( 1 ).toInt(), insertID );

            return;
         }

         // A listing a test canned wins over the stored records
         if ( ! listingFamily( proc ).isEmpty()  &&
              ! cannedFor.contains( proc + ":" + q.value( 1 ) )  &&
              ! canned.contains( proc ) )
         {
            int inv = q.value( 1 ).toInt();

            if ( ! admin  &&  inv != 0  &&  inv != person )
            {
               status = IUS_DB2::NOTPERMITTED;
               return;
            }

            listRecords( proc, inv );
            status = rows.isEmpty() ? int( IUS_DB2::NOROWS )
                                    : int( IUS_DB2::OK );
            return;
         }

         if ( proc == "get_experiment_desc" )
         {
            int        inv = q.value( 1 ).toInt();
            QList< int > ids = state.experiments.keys();

            if ( ! admin  &&  inv != person )
            {
               status = IUS_DB2::NOTPERMITTED;
               return;
            }

            for ( int ii = 0; ii < ids.size(); ii++ )
               if ( state.experiments[ ids[ ii ] ].owner == inv )
                  rows << ( QVariantList() << ids[ ii ]
                                           << state.experiments[ ids[ ii ] ].runID );
         }

         else if ( proc == "get_experiment_info" )
         {
            // The logged-in person's own runs, or any run for an admin
            int id = q.value( 1 ).toInt();

            if ( state.experiments.contains( id )  &&
                 ( admin  ||  state.experiments[ id ].owner == person ) )
               rows << ( QVariantList() << state.experiments[ id ].guid << 1
                                        << state.experiments[ id ].runID );
         }

         else if ( proc == "get_experiment_info_by_runID" )
         {
            int          inv = q.value( 2 ).toInt();
            QList< int > ids = state.experiments.keys();

            for ( int ii = 0; ii < ids.size(); ii++ )
            {
               const State::Experiment& exp = state.experiments[ ids[ ii ] ];

               if ( exp.runID == q.value( 1 )  &&  exp.owner == inv )
                  rows << ( QVariantList() << inv << ids[ ii ] << exp.guid );
            }
         }

         else if ( proc == "get_experiment_timestate" )
         {
            int id = q.value( 1 ).toInt();

            if ( state.timestates.contains( id ) )
               rows << ( QVariantList() << state.timestates[ id ] << id << "f"
                                        << "<TimeState/>" << "x" << "y" );
         }

         else if ( cannedFor.contains( proc + ":" + q.value( 1 ) ) )
            rows = cannedFor.value( proc + ":" + q.value( 1 ) );

         else if ( canned.contains( proc ) )
            rows = canned.value( proc );

         status = rows.isEmpty() ? int( IUS_DB2::NOROWS ) : int( IUS_DB2::OK );
      }
};
