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
 * GUID it is given under the record family of the procedure, and every
 * GUID lookup ("get_bufferID", "get_rawDataID_from_GUID", ...) finds what
 * was registered.  Listings are canned per procedure.  Transactions are
 * counted, and the log of calls says what happened in which order.
 *
 * A failure can be injected for one procedure (or blob procedure); the call
 * then reports DBERROR, as US_DB2 does for any MySQL error.
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
      int  commitAt  = -1;       // The log size when the commit came
      bool inTxn     = false;

      QString failOn;            // A procedure that fails with DBERROR

      // family -> GUID -> ID
      QMap< QString, QMap< QString, int > > registry;

      // experiment ID -> ( GUID, run ID ); timestate by experiment ID
      QMap< int, QPair< QString, QString > > experiments;
      QMap< int, int >           timestates;

      // procedure -> rows, for the listings a test sets up
      QMap< QString, QList< QVariantList > > canned;

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
               status = ( proc == failOn  ||  ! QFile::exists( file ) )
                        ? int( IUS_DB2::DBERROR ) : int( IUS_DB2::OK );
               return status;
            } ) );
         ON_CALL( mock, beginTransaction( _ ) )
            .WillByDefault( Invoke( [ this ]( QString& error )
            {
               error.clear();
               if ( inTxn )  return false;
               begins++;
               inTxn = true;
               return true;
            } ) );
         ON_CALL( mock, commitTransaction( _ ) )
            .WillByDefault( Invoke( [ this ]( QString& error )
            {
               error.clear();
               if ( ! inTxn )  return false;
               commits++;
               commitAt = log.size();
               inTxn    = false;
               return true;
            } ) );
         ON_CALL( mock, rollbackTransaction( _ ) )
            .WillByDefault( Invoke( [ this ]( QString& error )
            {
               error.clear();
               if ( ! inTxn )  return false;
               rollbacks++;
               inTxn = false;
               return true;
            } ) );
      }

      IUS_DB2* db() { return &mock; }

      // The calls of one procedure
      QList< QStringList > callsOf( const QString& proc ) const
      {
         QList< QStringList > found;

         for ( int ii = 0; ii < calls.size(); ii++ )
            if ( calls[ ii ][ 0 ] == proc )  found << calls[ ii ];

         return found;
      }

      // How many writes ("new_", "add_", "update_", "delete_", blobs) ran
      int writes() const
      {
         int knt = 0;

         for ( int ii = 0; ii < log.size(); ii++ )
            if ( isWrite( log[ ii ] ) )  knt++;

         return knt;
      }

      static bool isWrite( const QString& proc )
      {
         return proc.startsWith( "new_" )    ||  proc.startsWith( "add_" )  ||
                proc.startsWith( "create_" ) ||
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

         QString family = familyOfLookup( proc );

         if ( ! family.isEmpty() )
         {
            QString guid = q.value( 1 );

            if ( registry.value( family ).contains( guid ) )
            {
               rows << ( QVariantList() << registry[ family ][ guid ] );
               status = IUS_DB2::OK;
            }
            else
               status = IUS_DB2::NOROWS;

            return;
         }

         if ( isWrite( proc ) )
         {
            insertID = nextID++;
            status   = IUS_DB2::OK;
            family   = familyOfWrite( proc );

            for ( int ii = 1; ii < q.size()  &&  ! family.isEmpty(); ii++ )
            {
               if ( ! isGuid( q[ ii ] ) )  continue;

               registry[ family ].insert( q[ ii ], insertID );
               break;
            }

            if ( proc == "new_experiment" )
               experiments.insert( insertID, qMakePair( q.value( 1 ),
                                                        q.value( 3 ) ) );

            if ( proc == "new_timestate" )
               timestates.insert( q.value( 1 ).toInt(), insertID );

            return;
         }

         if ( proc == "get_experiment_desc" )
         {
            QList< int > ids = experiments.keys();

            for ( int ii = 0; ii < ids.size(); ii++ )
               rows << ( QVariantList() << ids[ ii ]
                                        << experiments[ ids[ ii ] ].second );
         }

         else if ( proc == "get_experiment_info" )
         {
            int id = q.value( 1 ).toInt();

            if ( experiments.contains( id ) )
               rows << ( QVariantList() << experiments[ id ].first << 1
                                        << experiments[ id ].second );
         }

         else if ( proc == "get_experiment_info_by_runID" )
         {
            QList< int > ids = experiments.keys();

            for ( int ii = 0; ii < ids.size(); ii++ )
               if ( experiments[ ids[ ii ] ].second == q.value( 1 ) )
                  rows << ( QVariantList() << 1 << ids[ ii ]
                                           << experiments[ ids[ ii ] ].first );
         }

         else if ( proc == "get_experiment_timestate" )
         {
            int id = q.value( 1 ).toInt();

            if ( timestates.contains( id ) )
               rows << ( QVariantList() << timestates[ id ] << id << "f"
                                        << "<TimeState/>" << "x" << "y" );
         }

         else if ( canned.contains( proc ) )
            rows = canned.value( proc );

         status = rows.isEmpty() ? int( IUS_DB2::NOROWS ) : int( IUS_DB2::OK );
      }
};
