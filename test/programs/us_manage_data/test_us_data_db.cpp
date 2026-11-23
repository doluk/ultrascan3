// test_us_data_db.cpp - us_manage_data against a database, through the mock
//
// The database half of the window -- listing and reading the store through
// the catalog, pairing database experiments and records with the local
// ones, comparing them, and the record actions -- runs here against a small
// fake server behind the IUS_DB2 mock.  It answers the stored procedures
// the catalog and the actions call from one data set, the way a server
// would, and keeps a log of what was asked, so a test can say both what the
// tree shows and what was never asked of the server.  The local half is a
// scratch results directory, as in test_us_data_model.cpp.
//
// The fake answers the way a server does (us3_sql is the reference, and
// test_us_data_db_live.cpp checks the parts that matter against a real
// one): a procedure that is missing or may not be run fails the CALL with
// server error 1305 or 1370, a lost connection with a client error (2013),
// a lock wait with 1205, and "no rows" is a procedure status with no server
// error at all -- or OK with an empty set, for the procedures that answer
// so.  The investigator is an ordinary user: the listings hold their own
// records only, a record of another investigator is refused (201) by the
// reads and deletes that check permission, and the GUID lookups are not
// scoped and ignore case.  Columns come back as US_DB2::value() gives them,
// as text decoded from UTF-8.  Writes change its data -- a removal removes,
// delete_editedData cascades as us3_expdata_procs.sql does and is refused
// while noise of another edit's model refers to the edit
// (fk_noise_editDataID is ON DELETE NO ACTION), keeping what it removed
// before; with patchedEditDelete it is all or nothing and also removes that
// noise, and commitsInside names writes that end the client's transaction
// (a COMMIT inside a procedure) -- new_noise
// files noise under edit 1 when the edit may not be used, transactions
// give back what was written since the begin unless the server ended them,
// and lastInsertID() never names the record just written.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <QtCore>
#include <QProgressBar>
#include <QLabel>
#include <QTreeWidget>
#include <QPointer>
#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
#include <QTextCodec>
#endif

#include "mock_us_db2.h"
#include "us_data_model.h"
#include "us_data_process.h"
#include "us_data_tree.h"
#include "us_settings.h"
#include "us_util.h"
#include "us_dataIO.h"
#include "us_model.h"
#include "us_noise.h"

using ::testing::_;
using ::testing::An;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

typedef US_DataModel::DataDesc Desc;

namespace
{
   const int     kInv   = 5;          // the investigator, an ordinary user
   const int     kOther = 6;          // another investigator
   const QString kDate  = "2024-03-04 05:06:07";

   // An experiment, and a model, belong to an investigator (experimentPerson,
   // modelPerson); raw data and edits through their experiment, a noise
   // record through its model
   struct FExp   { QString id, guid, runID; int owner = kInv; };
   struct FRaw   { QString id, guid, file, expID; };
   struct FEdit  { QString id, guid, rawID, file; };
   struct FModel { QString id, guid, desc, editGUID, editID, xml; int owner = kInv; };
   struct FNoise { QString id, guid, editID, modelID, modelGUID, type,
                   desc, xml; };

   struct Answer
   {
      int                   status = IUS_DB2::OK;
      int                   server = 0;       // the server's error number
      QString               error;
      QList< QVariantList > rows;
   };

   // The contents of a file, as text
   QString textOf( const QString& path )
   {
      QFile file( path );
      if ( ! file.open( QIODevice::ReadOnly ) )   return QString();
      return QString::fromUtf8( file.readAll() );
   }

   // GUIDs compare as the server's collation compares them: without case
   bool sameGuid( const QString& a, const QString& b )
   {
      return a.compare( b, Qt::CaseInsensitive ) == 0;
   }

   /* A server holding an investigator's store, and records of another one

      What the investigator may see and change follows us3_sql: an
      ordinary user is held to their own records, in the listings and the
      deletes alike, while the GUID lookups are not scoped. */
   class FakeServer
   {
      public:
         // What a transaction can give back
         struct Data
         {
            QList< FExp >   exps;
            QList< FRaw >   raws;
            QList< FEdit >  edits;
            QList< FModel > models;
            QList< FNoise > noises;
            QMap< QString, QString > digest;
            QMap< int, QByteArray >  blobs;
         };

         QList< FExp >   exps;
         QList< FRaw >   raws;
         QList< FEdit >  edits;
         QList< FModel > models;
         QList< FNoise > noises;

         QMap< QString, QString > digest;     // GUID -> "md5 size"
         QMap< int, QByteArray >  blobs;      // editedDataID -> contents
         QSet< QString >          failing;    // procedures whose call loses
                                              // the connection
         QMap< QString, int >     failTimes;  // ... the next n times only
         QMap< QString, int >     failAfter;  // ... every time after the
                                              // first n calls
         QMap< QString, int >     callCount;  // calls so far, by name
         QMap< QString, int >     transient;  // the next n calls wait for a
                                              // lock too long (1205)
         QSet< QString >          missing;    // procedures the server lacks
         QSet< QString >          denied;     // ... or may not be run
         QMap< QString, int >     refusing;   // writes refused with a status
         QStringList              calls;      // every query, by name
         QStringList              changes;    // every statusQuery
         QStringList              txlog;      // begin, commit, rollback
         int                      autoflow = 0;     // every run is GMP's
         QSet< QString >          autoflowRuns;     // ... or these
         QStringList              autoflowModels;   // model IDs GMP made
         QSet< QString >          commitsInside;    // writes that end the
                                                    // client's transaction
         int                      nextID   = 900;

         // delete_editedData as the patched us3_sql has it: all or nothing
         // (a savepoint in the caller's transaction), and all noise
         // recorded against the edit removed with it; a refusal changes
         // nothing.  Without it, as the procedure has long been: models and
         // their noise go first, and the edit then fails on
         // fk_noise_editDataID, keeping what went.
         bool                     patchedEditDelete = false;

         // The patched catalog procedures place a noise record with the
         // experiment of its model, not of the edit it was recorded against;
         // noise of a model on edit 1 belongs to no experiment
         bool                     noiseByModel = false;

         Answer current;
         int    row = -1;

         // The client's view of its transaction, and the server's
         bool   clientTx = false;
         bool   serverTx = false;
         Data   saved;

         Data data() const
         {
            return Data{ exps, raws, edits, models, noises, digest, blobs };
         }

         void restore( const Data& dd )
         {
            exps   = dd.exps;    raws   = dd.raws;   edits = dd.edits;
            models = dd.models;  noises = dd.noises;
            digest = dd.digest;  blobs  = dd.blobs;
         }

         void wire( NiceMock< US_DB2_Mock >& db )
         {
            ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
            ON_CALL( db, query( An< const QStringList& >() ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q ) { call( q ); } ) );
            ON_CALL( db, next() )
               .WillByDefault( Invoke( [ this ]() { return ++row < current.rows.size(); } ) );

            // As US_DB2::value() gives a column: the text of a C string the
            // server sent as UTF-8, a NULL as an empty string
            ON_CALL( db, value( _ ) )
               .WillByDefault( Invoke( [ this ]( unsigned ii ) -> QVariant
               {
                  if ( row < 0  ||  row >= current.rows.size() )  return QVariant();
                  if ( int( ii ) >= current.rows[ row ].size() )  return QVariant();
                  QByteArray bytes = current.rows[ row ].value( int( ii ) )
                                     .toString().toUtf8();
                  return QVariant( QString( bytes.constData() ) );
               } ) );
            ON_CALL( db, lastErrno() )
               .WillByDefault( Invoke( [ this ]() { return current.status; } ) );
            ON_CALL( db, lastError() )
               .WillByDefault( Invoke( [ this ]() { return current.error; } ) );
            ON_CALL( db, lastServerErrno() )
               .WillByDefault( Invoke( [ this ]() { return current.server; } ) );
            ON_CALL( db, lastInsertID() )
               .WillByDefault( Return( 4242 ) );     // never the record written
            ON_CALL( db, statusQuery( An< const QStringList& >() ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q )
               {
                  changes << q.join( " " );
                  change( q );
                  return current.status;
               } ) );
            ON_CALL( db, functionQuery( _ ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q )
               {
                  calls << q.value( 0 );
                  return autoflowRuns.contains( q.value( 1 ) ) ? 1 : autoflow;
               } ) );
            ON_CALL( db, readBlobFromDB( _, _, _ ) )
               .WillByDefault( Invoke( [ this ]( const QString& path,
                                                 const QString& proc, int id )
               {
                  calls << proc;
                  QFile file( path );
                  if ( ! blobs.contains( id )  ||
                       ! file.open( QIODevice::WriteOnly ) )
                     return int( IUS_DB2::DBERROR );
                  file.write( blobs[ id ] );
                  return int( IUS_DB2::OK );
               } ) );

            // Transactions as US_DB2 has them: a commit or a rollback of a
            // transaction the server ended already is refused
            ON_CALL( db, transactionActive() )
               .WillByDefault( Invoke( [ this ]() { return clientTx; } ) );
            ON_CALL( db, beginTransaction( _ ) )
               .WillByDefault( Invoke( [ this ]( QString& error )
               {
                  txlog << "begin";
                  error.clear();
                  if ( clientTx )  { error = "a transaction is open";  return false; }
                  clientTx = serverTx = true;
                  saved    = data();
                  return true;
               } ) );
            ON_CALL( db, commitTransaction( _ ) )
               .WillByDefault( Invoke( [ this ]( QString& error )
               {
                  txlog << "commit";
                  error.clear();
                  if ( ! clientTx )  { error = "no transaction";  return false; }
                  if ( ! serverTx )
                  {
                     error = "the transaction ended before the commit;"
                             " some rows may have been kept";
                     return false;
                  }
                  clientTx = serverTx = false;
                  return true;
               } ) );
            ON_CALL( db, rollbackTransaction( _ ) )
               .WillByDefault( Invoke( [ this ]( QString& error )
               {
                  txlog << "rollback";
                  error.clear();
                  if ( ! clientTx )  { error = "no transaction";  return false; }
                  clientTx = false;
                  if ( ! serverTx )
                  {
                     error = "the transaction ended before the rollback;"
                             " some rows may have been kept";
                     return false;
                  }
                  serverTx = false;
                  restore( saved );
                  return true;
               } ) );
         }

         int count( const QString& name ) const { return calls.count( name ); }

         QString ck( const QString& guid, int part ) const
         {
            QString text = digest.value( guid.simplified(),
                                         "md5-" + guid.simplified() + " 100" );
            return text.section( " ", part, part );
         }

         // How a call of a procedure fails, if it does; true when it did
         bool fails( const QString& name )
         {
            current = Answer();
            row     = -1;

            int ncall = ++callCount[ name ];

            if ( failTimes.value( name ) > 0  ||  failing.contains( name )  ||
                 ( failAfter.contains( name )  &&  ncall > failAfter.value( name ) ) )
            {
               if ( failTimes.value( name ) > 0 )  failTimes[ name ]--;
               current.status = IUS_DB2::DBERROR;
               current.server = 2013;
               current.error  = "Lost connection to MySQL server during query";
               return true;
            }

            if ( transient.value( name ) > 0 )
            {
               transient[ name ]--;
               current.status = IUS_DB2::DBERROR;
               current.server = 1205;
               current.error  = "Lock wait timeout exceeded; try restarting transaction";
               return true;
            }

            if ( missing.contains( name ) )
            {
               current.status = IUS_DB2::DBERROR;
               current.server = IUS_DB2::serverNoSuchRoutine;
               current.error  = "PROCEDURE us3." + name + " does not exist";
               return true;
            }

            if ( denied.contains( name ) )
            {
               current.status = IUS_DB2::DBERROR;
               current.server = IUS_DB2::serverRoutineDenied;
               current.error  = "execute command denied to user for routine us3." + name;
               return true;
            }

            return false;
         }

         void refuse( int status, const QString& error )
         {
            current.status = status;
            current.error  = error;
            current.rows.clear();
         }

         void call( const QStringList& q )
         {
            const QString name = q.value( 0 );
            calls << name;

            if ( fails( name ) )   return;

            answer( name, q.mid( 1 ) );
         }

         static QString md5Of( const QString& text )
         {
            QByteArray bytes = text.toUtf8();
            return QString( QCryptographicHash::hash( bytes, QCryptographicHash::Md5 )
                            .toHex() ) + " " + QString::number( bytes.size() );
         }

         // ---------------------------------------------- who may see what

         const FRaw* rawById( const QString& id ) const
         {
            for ( const FRaw& rr : raws )  if ( rr.id == id )  return &rr;
            return nullptr;
         }

         const FEdit* editById( const QString& id ) const
         {
            for ( const FEdit& ed : edits )  if ( ed.id == id )  return &ed;
            return nullptr;
         }

         const FModel* modelById( const QString& id ) const
         {
            for ( const FModel& mm : models )  if ( mm.id == id )  return &mm;
            return nullptr;
         }

         const FNoise* noiseById( const QString& id ) const
         {
            for ( const FNoise& nn : noises )  if ( nn.id == id )  return &nn;
            return nullptr;
         }

         QString expOfNoise( const FNoise& nn ) const
         {
            if ( ! noiseByModel )   return expOfEdit( nn.editID );
            const FModel* mm = modelById( nn.modelID );
            return mm ? expOfEdit( mm->editID ) : QString();
         }

         QString expOfEdit( const QString& editID ) const
         {
            const FEdit* ed = editById( editID );
            return ( ed  &&  rawById( ed->rawID ) ) ? rawById( ed->rawID )->expID
                                                    : QString();
         }

         bool expMine( const QString& expID ) const
         {
            for ( const FExp& ee : exps )
               if ( ee.id == expID )  return ee.owner == kInv;
            return false;
         }

         bool editMine( const QString& editID ) const
         {
            return expMine( expOfEdit( editID ) );
         }

         bool modelMine( const QString& modelID ) const
         {
            const FModel* mm = modelById( modelID );
            return mm  &&  mm->owner == kInv;
         }

         bool noiseMine( const FNoise& nn ) const
         {
            return modelMine( nn.modelID );
         }

         QString editIdOfGuid( const QString& guid ) const
         {
            for ( const FEdit& ed : edits )  if ( sameGuid( ed.guid, guid ) )  return ed.id;
            return "1";                      // the "unassigned" edit
         }

         QString rawGUIDOf( const FEdit& ed ) const
         {
            return rawById( ed.rawID ) ? rawById( ed.rawID )->guid : QString();
         }

         // ---------------------------------------------- writes

         // A write: what us3_sql's procedure does to the data
         void change( const QStringList& q )
         {
            const QString     name = q.value( 0 );
            const QStringList a    = q.mid( 1 );

            if ( fails( name ) )   return;

            // The patched procedure starts a transaction of its own, which
            // commits the client's before anything else happens
            if ( commitsInside.contains( name ) )
               serverTx = false;

            if ( refusing.contains( name ) )
            {
               refuse( refusing.value( name ), "MySQL: you do not have permission" );
               return;
            }

            if ( name == "delete_noise" )
            {  // verify_noise_permission
               const FNoise* nn = noiseById( a.value( 0 ) );
               if ( ! nn )            return refuse( IUS_DB2::NO_NOISE,
                                         "MySQL: the specified noise file does not exist" );
               if ( ! noiseMine( *nn ) ) return refuse( IUS_DB2::NOTPERMITTED,
                                         "MySQL: you do not have permission to view or modify this noise file" );
               for ( int ii = noises.size() - 1; ii >= 0; ii-- )
                  if ( noises[ ii ].id == a.value( 0 ) )  noises.removeAt( ii );
            }

            else if ( name == "delete_model" )
            {  // verify_model_permission; the model's noise goes with it
               if ( ! modelById( a.value( 0 ) ) )  return refuse( IUS_DB2::NO_MODEL,
                                         "MySQL: the specified model does not exist" );
               if ( ! modelMine( a.value( 0 ) ) )  return refuse( IUS_DB2::NOTPERMITTED,
                                         "MySQL: you do not have permission to view or modify this model" );
               for ( int ii = noises.size() - 1; ii >= 0; ii-- )
                  if ( noises[ ii ].modelID == a.value( 0 ) )  noises.removeAt( ii );
               for ( int ii = models.size() - 1; ii >= 0; ii-- )
                  if ( models[ ii ].id == a.value( 0 ) )  models.removeAt( ii );
            }

            else if ( name == "delete_editedData" )
            {
               const QString edit = a.value( 0 );

               // verify_editData_permission
               if ( expOfEdit( edit ).isEmpty() )  return refuse( 508,
                                         "MySQL: the associated experiment does not exist" );
               if ( ! editMine( edit ) )           return refuse( IUS_DB2::NOTPERMITTED,
                                         "MySQL: you do not have permission to view or modify this experiment" );

               // Every model of the edit, whoever's, and its noise wherever
               // it was recorded
               QSet< QString > onEdit;
               for ( const FModel& mm : models )
                  if ( mm.editID == edit )  onEdit << mm.id;

               for ( int ii = noises.size() - 1; ii >= 0; ii-- )
                  if ( onEdit.contains( noises[ ii ].modelID )  ||
                       ( patchedEditDelete  &&  noises[ ii ].editID == edit ) )
                     noises.removeAt( ii );
               for ( int ii = models.size() - 1; ii >= 0; ii-- )
                  if ( onEdit.contains( models[ ii ].id ) )  models.removeAt( ii );

               // Noise of another edit's model still refers to the edit:
               // the last statement fails, the ones before it stand
               for ( const FNoise& nn : noises )
                  if ( nn.editID == edit )
                  {
                     current.status = IUS_DB2::DBERROR;
                     current.server = 1451;
                     current.error  = "Cannot delete or update a parent row: a foreign"
                                      " key constraint fails (fk_noise_editDataID)";
                     return;
                  }

               for ( int ii = edits.size() - 1; ii >= 0; ii-- )
                  if ( edits[ ii ].id == edit )  edits.removeAt( ii );
            }

            else if ( name == "new_model" )
            {  // modelGUID, description, xml, variance, meniscus, editGUID, ownerID
               for ( const FModel& mm : models )
                  if ( sameGuid( mm.guid, a.value( 0 ) ) )
                     return refuse( 402, "MySQL: Duplicate entry for modelGUID field" );

               QString id = QString::number( ++nextID );
               FModel  mm{ id, a.value( 0 ), a.value( 1 ), a.value( 5 ),
                           editIdOfGuid( a.value( 5 ) ), a.value( 2 ) };
               mm.owner = a.value( 6 ).toInt();
               models << mm;
               digest[ a.value( 0 ) ] = md5Of( a.value( 2 ) );
            }

            else if ( name == "update_model" )
            {  // modelID, description, xml, variance, meniscus, editGUID
               if ( ! modelMine( a.value( 0 ) ) )
                  return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );

               for ( FModel& mm : models )
                  if ( mm.id == a.value( 0 ) )
                  {
                     mm.desc = a.value( 1 );
                     mm.xml  = a.value( 2 );
                     digest[ mm.guid ] = md5Of( mm.xml );
                  }
            }

            else if ( name == "new_noise" )
            {  // noiseGUID, editID, modelID, modelGUID, type, description, xml
               // An edit that may not be used is replaced by edit 1; the
               // model is found by GUID first, then by ID
               QString editID = editMine( a.value( 1 ) ) ? a.value( 1 ) : "1";
               const FModel* model = nullptr;

               for ( const FModel& mm : models )
                  if ( sameGuid( mm.guid, a.value( 3 ) ) )  model = &mm;
               if ( ! model )
                  model = modelById( a.value( 2 ) );

               if ( ! model )
                  return refuse( IUS_DB2::NO_MODEL, "MySQL: the model was not found in the database" );
               if ( model->owner != kInv )
                  return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );

               QString id = QString::number( ++nextID );
               noises << FNoise{ id, a.value( 0 ), editID, model->id, model->guid,
                                 a.value( 4 ), a.value( 5 ), a.value( 6 ) };
               digest[ a.value( 0 ) ] = md5Of( a.value( 6 ) );
            }

            else if ( name == "update_noise" )
            {  // noiseID, noiseGUID, editID, modelID, modelGUID, type, description, xml
               const FNoise* nn = noiseById( a.value( 0 ) );
               if ( ! nn  ||  ! noiseMine( *nn ) )
                  return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );

               for ( FNoise& no : noises )
                  if ( no.id == a.value( 0 ) )
                  {
                     no.desc = a.value( 6 );
                     no.xml  = a.value( 7 );
                     digest[ no.guid ] = md5Of( no.xml );
                  }
            }
         }

         // ---------------------------------------------- reads

         QVariantList rawRow( const FRaw& rr, bool hashed ) const
         {
            return QVariantList() << rr.id << rr.guid << "label" << "C:\\data\\" + rr.file
                                  << "comment" << rr.expID << "1" << kInv << kDate
                                  << ( hashed ? ck( rr.guid, 0 ) : QString() )
                                  << ( hashed ? ck( rr.guid, 1 ) : QString() );
         }

         QVariantList editRow( const FEdit& ed, bool hashed ) const
         {
            return QVariantList() << ed.id << ed.rawID << rawGUIDOf( ed ) << ed.guid
                                  << "label" << ed.file << "comment" << kDate
                                  << ( hashed ? ck( ed.guid, 0 ) : QString() )
                                  << ( hashed ? ck( ed.guid, 1 ) : QString() );
         }

         QVariantList modelRow( const FModel& mm ) const
         {
            return QVariantList() << mm.id << mm.guid << mm.desc << "" << ""
                                  << mm.editGUID << mm.editID << kDate
                                  << ck( mm.guid, 0 ) << ck( mm.guid, 1 );
         }

         QVariantList noiseRow( const FNoise& nn ) const
         {
            return QVariantList() << nn.id << nn.guid << nn.editID << nn.modelID
                                  << nn.type << nn.modelGUID << kDate
                                  << ck( nn.guid, 0 ) << ck( nn.guid, 1 ) << nn.desc;
         }

         // A procedure's answer: its status, and the rows that follow it
         void answer( const QString& name, const QStringList& a )
         {
            QList< QVariantList >& rows = current.rows;

            // These answer OK with an empty data set when there is nothing
            // (us3_catalog_procs.sql); every other one answers NOROWS
            static const QSet< QString > okWhenEmpty
            {
               "get_experiment_summary", "get_model_desc_by_experiment",
               "get_noise_desc_by_experiment", "get_rawData_by_person",
               "get_editedData_by_person"
            };

            if ( name == "get_experiment_summary" )
               for ( const FExp& ee : exps )
               {
                  if ( ee.owner != kInv )  continue;
                  int nr = 0, ne = 0, nm = 0, nn = 0;
                  for ( const FRaw& rr : raws )   if ( rr.expID == ee.id )  nr++;
                  for ( const FEdit& ed : edits ) if ( expOfEdit( ed.id ) == ee.id ) ne++;
                  for ( const FModel& mm : models )
                     if ( mm.owner == kInv  &&  expOfEdit( mm.editID ) == ee.id ) nm++;
                  for ( const FNoise& no : noises )
                     if ( noiseMine( no )  &&  expOfNoise( no ) == ee.id ) nn++;
                  rows << ( QVariantList() << ee.id << ee.guid << ee.runID << "velocity" << "RA"
                                           << ee.runID + " label" << "1" << ""
                                           << "project" << kDate << nr << ne << nm << nn );
               }

            else if ( name == "get_rawData_by_person" )
            {
               for ( const FRaw& rr : raws )
                  if ( expMine( rr.expID ) )  rows << rawRow( rr, false );
            }

            else if ( name == "get_editedData_by_person" )
            {
               for ( const FEdit& ed : edits )
                  if ( editMine( ed.id ) )  rows << editRow( ed, false );
            }

            else if ( name.endsWith( "_by_experiment" )  &&
                      ! expMine( a.value( name.startsWith( "get_model" )  ||
                                          name.startsWith( "get_noise" ) ? 1 : 0 ) ) )
               return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );

            else if ( name == "get_rawData_by_experiment" )
            {
               for ( const FRaw& rr : raws )
                  if ( rr.expID == a.value( 0 ) )  rows << rawRow( rr, true );
            }

            else if ( name == "get_editedData_by_experiment" )
            {
               for ( const FEdit& ed : edits )
                  if ( expOfEdit( ed.id ) == a.value( 0 ) )  rows << editRow( ed, true );
            }

            else if ( name == "get_model_desc" )
            {
               for ( const FModel& mm : models )
                  if ( mm.owner == kInv )  rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc" )
            {
               for ( const FNoise& nn : noises )
                  if ( noiseMine( nn ) )  rows << noiseRow( nn );
            }

            else if ( name == "get_model_desc_by_experiment" )
            {
               for ( const FModel& mm : models )
                  if ( mm.owner == kInv  &&  expOfEdit( mm.editID ) == a.value( 1 ) )
                     rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc_by_experiment" )
            {
               for ( const FNoise& nn : noises )
                  if ( noiseMine( nn )  &&  expOfNoise( nn ) == a.value( 1 ) )
                     rows << noiseRow( nn );
            }

            else if ( name == "get_rawData" )
            {
               const FRaw* rr = rawById( a.value( 0 ) );
               if ( rr  &&  ! expMine( rr->expID ) )
                  return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );
               if ( rr )
                  rows << ( QVariantList() << rr->guid << "label" << rr->file << "comment"
                                           << rr->expID << "1" << "" << kDate
                                           << ck( rr->guid, 0 ) << ck( rr->guid, 1 ) );
            }

            else if ( name == "get_editedData" )
            {  // NOROWS for no such edit, before the permission
               const FEdit* ed = editById( a.value( 0 ) );
               if ( ed  &&  ! editMine( ed->id ) )
                  return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );
               if ( ed )
                  rows << ( QVariantList() << ed->rawID << ed->guid << "label" << ed->file
                                           << "comment" << kDate << ck( ed->guid, 0 )
                                           << ck( ed->guid, 1 ) );
            }

            else if ( name == "get_model_info" )
            {  // verify_model_permission first
               const FModel* mm = modelById( a.value( 0 ) );
               if ( ! mm )            return refuse( IUS_DB2::NO_MODEL, "MySQL: the specified model does not exist" );
               if ( mm->owner != kInv ) return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );

               // modelGUID, description, xml, variance, meniscus, personID,
               // lastUpdated, checksum, size
               rows << ( QVariantList() << mm->guid << mm->desc << mm->xml << "0" << "0"
                                        << mm->owner << kDate << ck( mm->guid, 0 )
                                        << ck( mm->guid, 1 ) );
            }

            else if ( name == "get_noise_info" )
            {  // verify_noise_permission first
               const FNoise* nn = noiseById( a.value( 0 ) );
               if ( ! nn )               return refuse( IUS_DB2::NO_NOISE, "MySQL: the specified noise file does not exist" );
               if ( ! noiseMine( *nn ) ) return refuse( IUS_DB2::NOTPERMITTED, "MySQL: you do not have permission" );

               // noiseGUID, editedDataID, modelID, modelGUID, noiseType, xml,
               // timeEntered, checksum, size, description
               rows << ( QVariantList() << nn->guid << nn->editID << nn->modelID
                                        << nn->modelGUID << nn->type << nn->xml << kDate
                                        << ck( nn->guid, 0 ) << ck( nn->guid, 1 )
                                        << nn->desc );
            }

            // The GUID lookups are not scoped to the investigator
            else if ( name == "get_modelID" )
            {
               for ( const FModel& mm : models )
                  if ( sameGuid( mm.guid, a.value( 0 ) ) )  rows << ( QVariantList() << mm.id );
            }

            else if ( name == "get_noiseID" )
            {
               for ( const FNoise& nn : noises )
                  if ( sameGuid( nn.guid, a.value( 0 ) ) )  rows << ( QVariantList() << nn.id );
            }

            else if ( name == "get_rawDataID_from_GUID" )
            {
               for ( const FRaw& rr : raws )
                  if ( sameGuid( rr.guid, a.value( 0 ) ) )  rows << ( QVariantList() << rr.id );
            }

            // investigator, editedDataID: an ordinary user's own, whatever
            // investigator is named
            else if ( name == "get_model_desc_by_editID" )
            {
               for ( const FModel& mm : models )
                  if ( mm.owner == kInv  &&  mm.editID == a.value( 1 ) )
                     rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc_by_editID" )
            {
               for ( const FNoise& nn : noises )
                  if ( noiseMine( nn )  &&  nn.editID == a.value( 1 ) )
                     rows << noiseRow( nn );
            }

            else if ( name == "update_editedData" )
               rows << ( QVariantList() << IUS_DB2::OK );

            else if ( name == "get_modelIDs_for_autoflow" )
            {
               for ( const QString& id : autoflowModels )
                  rows << ( QVariantList()
                            << QString( "{\"2DSA_IT\":[{\"modelID\":\"%1\"}]}" ).arg( id ) );
            }

            if ( rows.isEmpty()  &&  ! okWhenEmpty.contains( name ) )
            {  // a procedure status, not a server error
               current.status = IUS_DB2::NOROWS;
               current.error  = "MySQL: no rows returned";
            }
         }
   };
}

class TestUSDataDb : public ::testing::Test
{
   protected:
      NiceMock< US_DB2_Mock > db;
      FakeServer              server;
      QString                 root;
      QString                 savedWorkDir;
      int                     savedInv;
      QProgressBar*           bar;
      QLabel*                 label;

      // The store both places share: RunA, with one triple and an edit, a
      // model and its noise, each the same record in both places
      const QString runA    = "RunA";
      QString       rawG, editG, modelG, noiseG, expG;
      QString       aucPath, editPath, modelPath, noisePath;

      void SetUp() override
      {
         savedWorkDir = US_Settings::workBaseDir();
         savedInv     = US_Settings::us_inv_ID();
         US_Settings::set_us_inv_ID( kInv );

         root = QDir::tempPath() + "/us_data_db_test_"
                + QString::number( QCoreApplication::applicationPid() ) + "_"
                + QString::number( QDateTime::currentMSecsSinceEpoch() );
         QDir().mkpath( root );
         US_Settings::set_workBaseDir( root );
         QDir().mkpath( US_Settings::dataDir() + "/models" );
         QDir().mkpath( US_Settings::dataDir() + "/noises" );

         server.wire( db );

         rawG   = US_Util::new_guid();
         editG  = US_Util::new_guid();
         modelG = US_Util::new_guid();
         noiseG = US_Util::new_guid();
         expG   = US_Util::new_guid();

         QString dir = runDir( runA );
         aucPath   = dir + "/" + runA + ".RA.1.A.280.auc";
         editPath  = dir + "/" + runA + ".2401011200.RA.1.A.280.xml";
         modelPath = US_Settings::dataDir() + "/models/M0000001.xml";
         noisePath = US_Settings::dataDir() + "/noises/N0000001.xml";

         writeAuc ( aucPath, rawG );
         writeEdit( editPath, editG, rawG );
         writeModel( modelPath, modelG, editG, runA + ".1A280.e2401011200_a1_2DSA_i01.model" );
         writeNoise( noisePath, noiseG, modelG, runA + ".1A280.e2401011200_a1_2DSA_i01.ti_noise" );

         server.exps   << FExp  { "1", expG, runA };
         server.raws   << FRaw  { "11", rawG, runA + ".RA.1.A.280.auc", "1" };
         server.edits  << FEdit { "21", editG, "11", runA + ".2401011200.RA.1.A.280.xml" };
         server.models << FModel{ "31", modelG, runA + ".1A280.e2401011200_a1_2DSA_i01.model",
                                  editG, "21", textOf( modelPath ) };
         server.noises << FNoise{ "41", noiseG, "21", "31", modelG, "ti_noise",
                                  runA + ".1A280.e2401011200_a1_2DSA_i01.ti_noise",
                                  textOf( noisePath ) };

         // The server's copies are the local ones
         server.digest[ rawG   ] = US_Util::md5sum_file( aucPath );
         server.digest[ editG  ] = US_Util::md5sum_file( editPath );
         server.digest[ modelG ] = US_Util::md5sum_file( modelPath );
         server.digest[ noiseG ] = US_Util::md5sum_file( noisePath );

         bar   = new QProgressBar();
         label = new QLabel();
      }

      void TearDown() override
      {
         delete bar;
         delete label;

         US_Settings::set_us_inv_ID( savedInv );
         US_Settings::set_workBaseDir( savedWorkDir );
         QDir( root ).removeRecursively();
      }

      QString runDir( const QString& runID )
      {
         QString path = US_Settings::resultDir() + "/" + runID;
         QDir().mkpath( path );
         return path;
      }

      void writeAuc( const QString& path, const QString& guid )
      {
         US_DataIO::RawData data;
         data.type[ 0 ]   = 'R';
         data.type[ 1 ]   = 'A';
         US_Util::uuid_parse( guid, (unsigned char*)data.rawGUID );
         data.cell        = 1;
         data.channel     = 'A';
         data.description = "triple";

         for ( int ii = 0; ii < 8; ii++ )  data.xvalues << 5.8 + 0.01 * ii;

         US_DataIO::Scan scan;
         scan.temperature = 20.0;
         scan.rpm         = 45000.0;
         scan.seconds     = 100.0;
         scan.omega2t     = 1.0e10;
         scan.wavelength  = 280.0;
         scan.plateau     = 0.5;
         scan.delta_r     = 0.01;
         scan.nz_stddev   = false;

         for ( int ii = 0; ii < 8; ii++ )
         { scan.rvalues << 0.1 * ii; scan.stddevs << 0.0; }

         scan.interpolated = QByteArray( 1, char( 0 ) );
         data.scanData << scan;

         ASSERT_EQ( US_DataIO::writeRawData( path, data ), US_DataIO::OK );
      }

      void writeEdit( const QString& path, const QString& editGUID,
                      const QString& rawGUID )
      {
         QString runID = path.section( "/", -1, -1 ).section( ".", 0, -7 );
         QFile   file( path );
         ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );

         QXmlStreamWriter xml( &file );
         xml.setAutoFormatting( true );
         xml.writeStartDocument();
         xml.writeDTD         ( "<!DOCTYPE UltraScanEdits>" );
         xml.writeStartElement( "experiment" );
         xml.writeAttribute   ( "type", "velocity" );
         xml.writeStartElement( "identification" );
         xml.writeStartElement( "runid" );
         xml.writeAttribute   ( "value", runID );
         xml.writeEndElement();
         xml.writeStartElement( "editGUID" );
         xml.writeAttribute   ( "value", editGUID );
         xml.writeEndElement();
         xml.writeStartElement( "rawDataGUID" );
         xml.writeAttribute   ( "value", rawGUID );
         xml.writeEndElement();
         xml.writeEndElement();
         xml.writeStartElement( "run" );
         xml.writeAttribute   ( "cell",       "1"   );
         xml.writeAttribute   ( "channel",    "A"   );
         xml.writeAttribute   ( "wavelength", "280" );
         xml.writeEndElement();
         xml.writeEndElement();
         xml.writeEndDocument();
         file.close();
      }

      void writeModel( const QString& path, const QString& guid,
                       const QString& editGUID, const QString& desc )
      {
         US_Model model;
         model.modelGUID   = guid;
         model.editGUID    = editGUID;
         model.description = desc;
         ASSERT_EQ( model.write( path ), US_DB2::OK );
      }

      void writeNoise( const QString& path, const QString& guid,
                       const QString& modelGUID, const QString& desc )
      {
         US_Noise noise;
         noise.noiseGUID   = guid;
         noise.modelGUID   = modelGUID;
         noise.description = desc;
         noise.type        = US_Noise::TI;
         noise.values << 0.001 << 0.002;
         noise.count       = 2;
         ASSERT_EQ( noise.write( path ), US_DB2::OK );
      }

      void prepare( US_DataModel& model, const QString& run = "ALL",
                    const QString& source = "ALL" )
      {
         model.setDatabase( &db );
         model.setProgress( bar, label );
         model.setFilters ( run, "ALL", source );
      }

      // The row of the record with a GUID, in an experiment's range or in
      // the whole tree
      int rowOf( US_DataModel& model, const QString& guid, int runIndex = -1 )
      {
         for ( int row = 0; row < model.recCount(); row++ )
         {
            if ( model.row_datadesc( row ).dataGUID != guid )          continue;
            if ( runIndex >= 0  &&  model.run_of_row( row ) != runIndex ) continue;
            return row;
         }

         return -1;
      }

      int stateOf( US_DataModel& model, const QString& guid )
      {
         int row = rowOf( model, guid );
         return ( row < 0 ) ? -1 : model.row_datadesc( row ).recState;
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

      int standIn( US_DataModel& model, bool globals )
      {
         for ( int ii = 0; ii < model.runCount(); ii++ )
         {
            US_DataModel::RunEntry entry = model.run_entry( ii );
            if ( entry.orphans  &&  entry.globals == globals )  return ii;
         }

         return -1;
      }
};

// A tree, a processor and the prompts they would have shown
struct Window
{
   QTreeWidget    tw;
   US_DataProcess proc;
   US_DataTree    tree;
   QStringList    prompts;
   bool           answer = true;

   explicit Window( US_DataModel& model )
      : proc( &model ), tree( &model, &tw )
   {
      tw.setSelectionMode( QAbstractItemView::ExtendedSelection );
      model.setSiblings( &proc, &tree );
      tree.setPrompter( [ this ]( int, const QString& title, const QString& text )
                        { prompts << title + ": " + text; return answer; } );
   }

   void select( QTreeWidgetItem* item )
   {
      tw.clearSelection();
      ASSERT_NE( item, nullptr );
      item->setSelected( true );
   }
};

// ------------------------------------------------------------ the scan

// One experiment in both places: its records pair by GUID, the scan reads
// the store in its whole-store queries, and compares the experiment
TEST_F( TestUSDataDb, RecordsInBothPlacesArePairedAndCompared )
{
   US_DataModel model;
   prepare( model );

   model.scan_data();

   ASSERT_EQ( model.runCount(), 1 );
   US_DataModel::RunEntry entry = model.run_entry( 0 );
   EXPECT_TRUE( entry.inBoth() );
   EXPECT_TRUE( entry.loaded );
   EXPECT_TRUE( entry.verified );
   EXPECT_TRUE( model.scan_errors().isEmpty() )
      << model.scan_errors().join( "\n" ).toStdString();
   EXPECT_FALSE( model.db_listing_failed() );

   // the whole store in its own queries, not one experiment at a time
   EXPECT_EQ( server.count( "get_rawData_by_person" ), 1 );
   EXPECT_EQ( server.count( "get_model_desc" ), 1 );
   EXPECT_EQ( server.count( "get_model_desc_by_experiment" ), 0 );

   for ( const QString& guid : { rawG, editG, modelG, noiseG } )
   {
      int row = rowOf( model, guid );
      ASSERT_GE( row, 0 );
      EXPECT_EQ( model.run_of_row( row ), 0 );
      EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
                 US_DataModel::SYNC_INSYNC )
         << model.row_datadesc( row ).contents.toStdString();
   }

   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( entry.row ) ),
              US_DataModel::SYNC_INSYNC );
   EXPECT_TRUE( model.find_duplicates().isEmpty() );
   EXPECT_EQ( model.recCountDB(),  4 );
   EXPECT_EQ( model.recCountLoc(), 4 );
}

// Copies that differ are a conflict, and the experiment row says so
TEST_F( TestUSDataDb, CopiesThatDifferAreAConflict )
{
   server.digest[ editG ] = "0123456789abcdef0123456789abcdef 999";

   US_DataModel model;
   prepare( model );
   model.scan_data();

   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, editG ) ) ),
              US_DataModel::SYNC_CONFLICT );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, rawG ) ) ),
              US_DataModel::SYNC_INSYNC );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( model.run_entry( 0 ).row ) ),
              US_DataModel::SYNC_CONFLICT );
}

// Two database experiments may share a run ID: each is an experiment of its
// own, and the local run joins the first
TEST_F( TestUSDataDb, DatabaseExperimentsSharingARunIdAreKeptApart )
{
   server.exps << FExp{ "2", US_Util::new_guid(), runA };

   US_DataModel model;
   prepare( model );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 2 );
   US_DataModel::RunEntry first  = model.run_entry( 0 );
   US_DataModel::RunEntry second = model.run_entry( 1 );

   EXPECT_EQ( first .runID.toStdString(), runA.toStdString() );
   EXPECT_EQ( second.runID.toStdString(), runA.toStdString() );
   EXPECT_TRUE ( first.inBoth() );
   EXPECT_FALSE( second.inBoth() );
   EXPECT_GE   ( second.dbIndex, 0 );
   EXPECT_NE   ( first.dbIndex, second.dbIndex );

   // the records are the first one's; the second has none
   EXPECT_EQ( model.run_of_row( rowOf( model, editG ) ), 0 );
   EXPECT_TRUE( model.descendant_rows( second.row ).isEmpty() );
}

// A run filter reads that run alone: no whole-store query goes out
TEST_F( TestUSDataDb, AFilteredScanReadsOnlyTheFilteredRun )
{
   QString rawB = US_Util::new_guid();
   server.exps << FExp{ "2", US_Util::new_guid(), "RunB" };
   server.raws << FRaw{ "12", rawB, "RunB.RA.1.A.280.auc", "2" };

   US_DataModel model;
   prepare( model, runA );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_EQ( model.run_entry( 0 ).runID.toStdString(), runA.toStdString() );

   for ( const char* proc : { "get_rawData_by_person", "get_editedData_by_person",
                              "get_model_desc", "get_noise_desc",
                              "all_rawDataIDs", "all_editedDataIDs" } )
      EXPECT_EQ( server.count( proc ), 0 ) << proc;

   EXPECT_GE( server.count( "get_rawData_by_experiment" ), 1 );
   EXPECT_EQ( rowOf( model, rawB ), -1 );

   // and the run is still compared
   EXPECT_TRUE( model.run_entry( 0 ).verified );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, editG ) ) ),
              US_DataModel::SYNC_INSYNC );
}

// When the database cannot be listed the window says so, and nothing else
// is asked of it: every record would read as an orphan
TEST_F( TestUSDataDb, AFailedListingIsReportedAndReadsNothingMore )
{
   server.failing << "get_experiment_summary" << "get_experiment_desc";

   US_DataModel model;
   prepare( model );
   model.scan_data();

   EXPECT_TRUE ( model.db_listing_failed() );
   EXPECT_FALSE( model.scan_errors().isEmpty() );
   EXPECT_EQ   ( server.count( "get_rawData_by_person" ), 0 );
   EXPECT_EQ   ( server.count( "get_model_desc" ), 0 );

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_FALSE( model.run_entry( 0 ).inBoth() );
   EXPECT_EQ( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
   EXPECT_LT( standIn( model, false ), 0 );
}

// ------------------------------------------------------------ orphans

// A database record whose parent is gone, and a global model, which has no
// edit by design, are each under a stand-in of their own
TEST_F( TestUSDataDb, DatabaseOrphansAndGlobalModelsHaveStandIns )
{
   QString lost = US_Util::new_guid();
   QString glob = US_Util::new_guid();
   server.models << FModel{ "32", lost, runA + ".1A280.lost.model",
                            US_Util::new_guid(), "99", "" };
   server.models << FModel{ "33", glob, "Global.global.e1_a1_2DSA-GL_i01.model",
                            "", "", "" };

   US_DataModel model;
   prepare( model );

   QList< int > added;
   QObject::connect( &model, &US_DataModel::orphansAdded,
                     [ & ]( int index ) { added << index; } );

   model.scan_data();

   int orph = standIn( model, false );
   int glbs = standIn( model, true );
   ASSERT_GE( orph, 0 );
   ASSERT_GE( glbs, 0 );
   EXPECT_EQ( added, QList< int >() << orph << glbs );

   EXPECT_EQ( model.run_of_row( rowOf( model, lost ) ), orph );
   EXPECT_EQ( model.run_of_row( rowOf( model, glob ) ), glbs );
   EXPECT_EQ( stateOf( model, lost ) & US_DataModel::REC_DB, (int)US_DataModel::REC_DB );

   // stand-ins are not experiments
   EXPECT_EQ( model.runCount(), 3 );
   EXPECT_EQ( model.experimentCount(), 1 );
}

// A local copy of a database record whose parent is in the database only
// pairs with the database record, under that parent -- not as an orphan,
// and not as a duplicate
TEST_F( TestUSDataDb, ALocalCopyOfARecordWithADatabaseOnlyParentPairsWithIt )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString mpath  = US_Settings::dataDir() + "/models/M0000002.xml";
   writeModel( mpath, model2, edit2, runA + ".1A280.e2401021200_a1_2DSA_i01.model" );

   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, runA + ".1A280.e2401021200_a1_2DSA_i01.model",
                            edit2, "22", textOf( mpath ) };
   server.digest[ model2 ] = US_Util::md5sum_file( mpath );

   US_DataModel model;
   prepare( model );
   model.scan_data();

   int row = rowOf( model, model2 );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( model.run_of_row( row ), 0 );
   EXPECT_EQ( model.row_datadesc( row ).recState
              & ( US_DataModel::REC_DB | US_DataModel::REC_LO ),
              US_DataModel::REC_DB | US_DataModel::REC_LO );
   EXPECT_EQ( model.row_datadesc( model.parent_row( row ) ).dataGUID.toStdString(),
              edit2.toStdString() );

   // one row for the record, nothing left as an orphan, no false alarm
   int nrows = 0;
   for ( int rr = 0; rr < model.recCount(); rr++ )
      nrows += ( model.row_datadesc( rr ).dataGUID == model2 ) ? 1 : 0;
   EXPECT_EQ( nrows, 1 );
   EXPECT_LT( standIn( model, false ), 0 );
   EXPECT_TRUE( model.find_duplicates().isEmpty() );

   // and it is compared like any other
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC )
      << model.row_datadesc( row ).contents.toStdString();
}

// The records of a stand-in that are in both places are compared too, and
// the stand-in row is no record of either place
TEST_F( TestUSDataDb, AStandInsPairedRecordsAreCompared )
{
   QString lostEdit = US_Util::new_guid();
   QString lost     = US_Util::new_guid();
   QString mpath    = US_Settings::dataDir() + "/models/M0000009.xml";
   writeModel( mpath, lost, lostEdit, runA + ".1A280.e9_a9_2DSA_i01.model" );
   server.models << FModel{ "39", lost, runA + ".1A280.e9_a9_2DSA_i01.model",
                            lostEdit, "99", textOf( mpath ) };
   server.digest[ lost ] = US_Util::md5sum_file( mpath );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int orph = standIn( model, false );
   ASSERT_GE( orph, 0 );
   int row = rowOf( model, lost, orph );
   ASSERT_GE( row, 0 );

   EXPECT_TRUE( model.run_entry( orph ).verified );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC )
      << model.row_datadesc( row ).contents.toStdString();
   EXPECT_FALSE( model.needs_verify( orph ) );

   // neutral: neither "DB" nor "Local" nor a sync state
   EXPECT_EQ( win.tw.topLevelItem( orph )->text( 3 ).toStdString(), "--" );
}

// ------------------------------------------------------------ comparisons

// An experiment asked for while the chain is read is compared first
TEST_F( TestUSDataDb, AnExperimentAskedForIsComparedFirst )
{
   QString rawB = US_Util::new_guid();
   QString aucB = runDir( "RunB" ) + "/RunB.RA.1.A.280.auc";
   writeAuc( aucB, rawB );
   server.exps << FExp{ "2", US_Util::new_guid(), "RunB" };
   server.raws << FRaw{ "12", rawB, "RunB.RA.1.A.280.auc", "2" };
   server.digest[ rawB ] = US_Util::md5sum_file( aucB );

   US_DataModel model;
   prepare( model );

   bool asked = false;
   QList< int > compared;
   QObject::connect( &model, &US_DataModel::runMerged, [ & ]( int )
   {
      if ( asked )   return;
      asked = true;
      model.request_verify( model.index_of_run( "RunB" ) );
   } );
   QObject::connect( &model, &US_DataModel::runCompared,
                     [ & ]( int index ) { compared << index; } );

   model.scan_data();

   ASSERT_EQ( compared.size(), 2 );
   EXPECT_EQ( compared.first(), model.index_of_run( "RunB" ) );
}

// The checksums are found whatever whitespace the server leaves around a
// GUID: the rows carry it simplified
TEST_F( TestUSDataDb, ChecksumsAreFoundByTheSimplifiedGuid )
{
   server.models[ 0 ].guid = modelG + "  ";
   server.noises[ 0 ].modelGUID = modelG + "  ";
   server.failing << "get_model_info";     // nothing to fall back on

   US_DataModel model;
   prepare( model );
   model.scan_data();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC )
      << model.row_datadesc( row ).contents.toStdString();
}

// A model whose two copies differ only in how they are written out shows in
// sync, and so does its experiment
TEST_F( TestUSDataDb, TheExperimentRowReadsAsItsRecordsDo )
{
   server.models[ 0 ].xml += "\n\n";
   QByteArray xml = server.models[ 0 ].xml.toUtf8();
   server.digest[ modelG ] = QString( QCryptographicHash::hash(
                                xml, QCryptographicHash::Md5 ).toHex() )
                             + " " + QString::number( xml.size() );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_CONFLICT );            // the bytes differ

   QTreeWidgetItem* item = itemOfRow( win.tw, row );
   ASSERT_NE( item, nullptr );
   EXPECT_EQ( item->text( 3 ).toStdString(), "In Sync" );   // the model does not
   EXPECT_EQ( win.tw.topLevelItem( 0 )->text( 3 ).toStdString(), "In Sync" );
}

// ------------------------------------------------------------ actions

// A record downloaded is compared again, on its own, in the background
TEST_F( TestUSDataDb, ADownloadedRecordIsComparedAgain )
{
   QString edit3 = US_Util::new_guid();
   QString file3 = runA + ".2401031200.RA.1.A.280.xml";
   QString tmp   = root + "/blob.xml";
   writeEdit( tmp, edit3, rawG );
   server.edits << FEdit{ "23", edit3, "11", file3 };
   server.blobs[ 23 ]      = textOf( tmp ).toUtf8();
   server.digest[ edit3 ]  = US_Util::md5sum_file( tmp );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   ASSERT_TRUE( model.run_entry( 0 ).verified );
   int row = rowOf( model, edit3 );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( model.row_datadesc( row ).recState & US_DataModel::REC_LO, 0 );

   int asked = 0;
   QObject::connect( &model, &US_DataModel::workRequested, [ & ]() { asked++; } );

   win.select( itemOfRow( win.tw, row ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "Local file create" ), 0 )
      << win.proc.lastError().toStdString();

   EXPECT_TRUE( QFile::exists( runDir( runA ) + "/" + file3 ) );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_UNVERIFIED );
   EXPECT_FALSE( model.run_entry( 0 ).verified );
   EXPECT_EQ( win.tw.topLevelItem( 0 )->text( 3 ).toStdString(), "Unverified" );
   EXPECT_GT( asked, 0 );
   EXPECT_EQ( model.next_pending_verify(), 0 );

   // the background pass compares that record, and only that record
   int before = server.count( "get_editedData_by_experiment" );
   EXPECT_TRUE( model.pump_once() );
   EXPECT_EQ( server.count( "get_editedData_by_experiment" ), before );
   EXPECT_EQ( server.count( "get_editedData" ), 1 );

   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC );
   EXPECT_TRUE( model.run_entry( 0 ).verified );
   EXPECT_FALSE( model.pump_once() );
}

// A database replace of a copy the database already holds writes nothing
TEST_F( TestUSDataDb, UploadingWhatTheDatabaseHoldsIsNothingToDo )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int row = rowOf( model, editG );
   ASSERT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC );

   win.select( itemOfRow( win.tw, row ) );
   EXPECT_EQ( win.tree.act_on_selection( "both DB and Local", "DB replace" ),
              US_DataTree::ACT_IN_SYNC );
   EXPECT_EQ( server.count( "update_editedData" ), 0 );
   EXPECT_EQ( server.count( "new_editedData" ), 0 );

   // Not compared yet, and the database copy is newer -- it came from this
   // very file: the contents decide, not the dates
   Desc desc = model.row_datadesc( row );
   desc.recState   &= ~US_DataModel::HV_DET;
   desc.contents    = QString();
   desc.lastmodDate = "2030-01-01 00:00:00 UTC";
   desc.filemodDate = "2020-01-01 00:00:00 UTC";
   model.change_datadesc( desc, row );

   EXPECT_EQ( win.proc.record_upload( row ), (int)US_DataProcess::IN_SYNC );
   EXPECT_EQ( server.count( "get_editedData" ), 1 );     // compared on its own

   // When they differ, an older local copy does not replace the database's
   QFile file( editPath );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( "\n" );
   file.close();

   EXPECT_EQ( win.proc.record_upload( row ), 3061 );
   EXPECT_TRUE( win.proc.lastError().contains( "older" ) );
   EXPECT_EQ( server.count( "update_editedData" ), 0 );
}

// Removing an edit from the database removes what hangs off it there too;
// those rows are marked, not deleted again
TEST_F( TestUSDataDb, RemovingFromTheDatabaseCascades )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   win.select( itemOfRow( win.tw, rowOf( model, editG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 )
      << win.proc.lastError().toStdString();

   EXPECT_EQ( server.changes, QStringList() << "delete_editedData 21" );

   for ( const QString& guid : { editG, modelG, noiseG } )
   {
      Desc desc = model.row_datadesc( rowOf( model, guid ) );
      EXPECT_EQ( desc.recState & US_DataModel::REC_DB, 0 );
      EXPECT_NE( desc.recState & US_DataModel::REC_LO, 0 );
      EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_LOCAL );
   }

   EXPECT_NE( stateOf( model, rawG ) & US_DataModel::REC_DB, 0 );

   // the database really lost them, and the counts say what is left: the
   // edit's model is local only, and the raw is all the database still has
   EXPECT_TRUE( server.edits.isEmpty() );
   EXPECT_TRUE( server.models.isEmpty() );
   EXPECT_TRUE( server.noises.isEmpty() );

   QTreeWidgetItem* eitem = itemOfRow( win.tw, rowOf( model, editG ) );
   ASSERT_NE( eitem, nullptr );
   EXPECT_EQ( eitem->text( 4 ).toStdString(), "0, 1" );
   EXPECT_EQ( eitem->text( 5 ).toStdString(), "0, 2" );
   EXPECT_EQ( model.run_entry( 0 ).dbCount, 1 );
   EXPECT_EQ( model.run_entry( 0 ).loCount, 4 );
   EXPECT_EQ( win.tw.topLevelItem( 0 )->text( 5 ).toStdString(), "1, 4" );
   EXPECT_EQ( model.count_records().db[ US_DataModel::EDIT ], 0 );
}

// Nothing the GMP framework needs is removed, and nothing is asked to be
TEST_F( TestUSDataDb, GmpDataIsNotRemoved )
{
   server.autoflow = 1;

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   win.select( win.tw.topLevelItem( 0 ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_REFUSED );
   EXPECT_TRUE( server.changes.isEmpty() );
   ASSERT_EQ( win.prompts.size(), 1 );
   EXPECT_TRUE( win.prompts[ 0 ].contains( "GMP" ) );
   EXPECT_NE( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
}

// Removing everything under the global models asks first, with the count
TEST_F( TestUSDataDb, RemovingAllGlobalModelsAsksFirst )
{
   server.models << FModel{ "51", US_Util::new_guid(), "G.global.e1_a1_2DSA-GL_i01.model", "", "", "" };
   server.models << FModel{ "52", US_Util::new_guid(), "G.global.e1_a1_2DSA-GL_i02.model", "", "", "" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int glbs = standIn( model, true );
   ASSERT_GE( glbs, 0 );
   win.select( win.tw.topLevelItem( glbs ) );

   win.answer = false;
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_CANCELLED );
   EXPECT_TRUE( server.changes.isEmpty() );
   ASSERT_EQ( win.prompts.size(), 1 );
   EXPECT_TRUE( win.prompts[ 0 ].contains( "all 2 record(s)" ) )
      << win.prompts[ 0 ].toStdString();

   win.answer = true;
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 );
   EXPECT_EQ( server.changes, QStringList() << "delete_model 51" << "delete_model 52" );

   // the experiment's own model is not among them
   EXPECT_NE( stateOf( model, modelG ) & US_DataModel::REC_DB, 0 );
}

// A download that would land in another directory than the run's is refused
TEST_F( TestUSDataDb, NothingIsWrittenForARunInTheWrongDirectory )
{
   QDir().rename( runDir( runA ), US_Settings::resultDir() + "/Other_dir" );
   QString edit3 = US_Util::new_guid();
   server.edits << FEdit{ "23", edit3, "11", runA + ".2401031200.RA.1.A.280.xml" };
   server.blobs[ 23 ] = "x";

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_TRUE( model.run_entry( 0 ).unsafe );

   win.select( itemOfRow( win.tw, rowOf( model, edit3 ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "Local file create" ),
              US_DataTree::ACT_REFUSED );
   EXPECT_FALSE( QDir( runDir( runA ) ).exists( runA + ".2401031200.RA.1.A.280.xml" ) );
   EXPECT_EQ( server.count( "download_editData" ), 0 );

   // and the processor refuses on its own too
   EXPECT_EQ( win.proc.record_download( rowOf( model, edit3 ) ),
              (int)US_DataProcess::UNSAFE_RUN );
}

// A model is not downloaded where its edit is not
TEST_F( TestUSDataDb, AChildIsNotDownloadedWithoutItsParent )
{
   QString edit3  = US_Util::new_guid();
   QString model3 = US_Util::new_guid();
   server.edits  << FEdit { "23", edit3, "11", runA + ".2401031200.RA.1.A.280.xml" };
   server.models << FModel{ "35", model3, runA + ".1A280.e2401031200_a1_2DSA_i01.model",
                            edit3, "23", "<xml/>" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, model3 );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( win.proc.record_download( row ), (int)US_DataProcess::NO_PARENT );
   EXPECT_EQ( server.count( "get_model_info" ), 0 );
}

// ------------------------------------------------------------ descriptions

// An older server cuts model descriptions at 80 characters and leaves
// noise descriptions empty; the catalog repairs them, and the tree shows
// what it repaired
TEST_F( TestUSDataDb, DescriptionsAnOlderServerCutAreShownInFull )
{
   QString full = runA + ".1A280.e2401011200_a2401011300_2DSA-FM-IT_"
                  "with_a_description_long_enough_to_be_cut_by_the_160_character"
                  "_description_column_of_the_model_table_in_the_database_i01.model";
   ASSERT_GT( full.length(), 160 );

   QString cutG  = US_Util::new_guid();
   QString cutN  = US_Util::new_guid();
   QString mpath = root + "/cut.xml";
   writeModel( mpath, cutG, editG, full );

   server.models << FModel{ "36", cutG, full.left( 160 ), editG, "21",
                            textOf( mpath ) };
   server.noises << FNoise{ "46", cutN, "21", "36", cutG, "ri_noise", "", "" };

   US_DataModel model;
   prepare( model );
   model.scan_data();

   int mrow = rowOf( model, cutG );
   int nrow = rowOf( model, cutN );
   ASSERT_GE( mrow, 0 );
   ASSERT_GE( nrow, 0 );

   EXPECT_EQ( model.row_datadesc( mrow ).description.toStdString(),
              full.toStdString() );
   EXPECT_LE( model.row_datadesc( mrow ).label.length(), 40 );
   EXPECT_EQ( model.row_datadesc( nrow ).description.toStdString(),
              QString( full ).replace( ".model", ".ri_noise" ).toStdString() );
}

// ------------------------------------------------------------ record IDs

// A DB replace of a model updates the record the row names, and the row
// keeps its ID -- not whatever lastInsertID() says -- so a later removal
// removes that record
TEST_F( TestUSDataDb, AReplacedModelKeepsItsDatabaseId )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   // the local copy changes, so there is something to replace
   writeModel( modelPath, modelG, editG,
               runA + ".1A280.e2401011200_a1_2DSA_i01.model" );
   QFile file( modelPath );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( "\n" );
   file.close();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );
   ASSERT_EQ( model.row_datadesc( row ).recordID, 31 );

   Desc desc = model.row_datadesc( row );        // not compared since
   desc.recState &= ~US_DataModel::HV_DET;
   desc.contents  = QString();
   model.change_datadesc( desc, row );

   EXPECT_EQ( win.proc.record_upload( row ), 0 ) << win.proc.lastError().toStdString();
   ASSERT_FALSE( server.changes.isEmpty() );
   EXPECT_TRUE( server.changes.first().startsWith( "update_model 31 " ) )
      << server.changes.join( "\n" ).toStdString();
   EXPECT_EQ( model.row_datadesc( row ).recordID, 31 );

   server.changes.clear();
   win.select( itemOfRow( win.tw, row ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 )
      << win.proc.lastError().toStdString();
   EXPECT_EQ( server.changes, QStringList() << "delete_model 31" );
}

// A model and a noise uploaded for the first time get the IDs of the
// records created for their GUIDs
TEST_F( TestUSDataDb, NewModelAndNoiseGetTheIdsOfTheirRecords )
{
   QString newM  = US_Util::new_guid();
   QString newN  = US_Util::new_guid();
   QString mdesc = runA + ".1A280.e2401011200_a2_2DSA_i01.model";
   writeModel( US_Settings::dataDir() + "/models/M0000002.xml", newM, editG, mdesc );
   writeNoise( US_Settings::dataDir() + "/noises/N0000002.xml", newN, newM,
               runA + ".1A280.e2401011200_a2_2DSA_i01.ti_noise" );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int mrow = rowOf( model, newM );
   ASSERT_GE( mrow, 0 );
   ASSERT_EQ( model.row_datadesc( mrow ).recState & US_DataModel::REC_DB, 0 );

   win.select( itemOfRow( win.tw, mrow ) );
   EXPECT_EQ( win.tree.act_on_selection( "Local disk only", "DB create" ), 0 )
      << win.proc.lastError().toStdString();

   // the model first, then its noise, each under the ID made for its GUID
   QString modelID;
   QString noiseID;
   for ( const FModel& mm : server.models )  if ( mm.guid == newM )  modelID = mm.id;
   for ( const FNoise& nn : server.noises )  if ( nn.guid == newN )  noiseID = nn.id;
   ASSERT_FALSE( modelID.isEmpty() );
   ASSERT_FALSE( noiseID.isEmpty() );

   int nrow = rowOf( model, newN );
   EXPECT_EQ( model.row_datadesc( mrow ).recordID, modelID.toInt() );
   EXPECT_EQ( model.row_datadesc( nrow ).recordID, noiseID.toInt() );
   EXPECT_NE( model.row_datadesc( mrow ).recState & US_DataModel::REC_DB, 0 );

   // and the comparison that follows reads those records, and finds them
   // the same as the files
   while ( model.pump_once() ) {}
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( mrow ) ),
              US_DataModel::SYNC_INSYNC ) << model.scan_errors().join( "\n" ).toStdString();
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( nrow ) ),
              US_DataModel::SYNC_INSYNC ) << model.scan_errors().join( "\n" ).toStdString();

   server.changes.clear();
   win.select( itemOfRow( win.tw, nrow ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 );
   EXPECT_EQ( server.changes, QStringList() << "delete_noise " + noiseID );
}

// A local model that cannot be read is not uploaded as an empty one
TEST_F( TestUSDataDb, AnUnreadableLocalModelIsNotUploaded )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );

   QFile file( modelPath );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( "this is no model" );
   file.close();

   Desc desc = model.row_datadesc( row );       // not compared since
   desc.recState &= ~US_DataModel::HV_DET;
   desc.contents  = QString();
   model.change_datadesc( desc, row );

   EXPECT_EQ( win.proc.record_upload( row ), 3046 );
   EXPECT_TRUE( win.proc.lastError().contains( modelPath ) )
      << win.proc.lastError().toStdString();
   EXPECT_TRUE( server.changes.isEmpty() ) << server.changes.join( "\n" ).toStdString();
   EXPECT_EQ( textOf( modelPath ).toStdString(), "this is no model" );
}

// A row whose database ID is not known is not removed by another one
TEST_F( TestUSDataDb, ARowWithoutADatabaseIdIsNotRemoved )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int  row  = rowOf( model, noiseG );
   Desc desc = model.row_datadesc( row );
   desc.recordID = -1;
   model.change_datadesc( desc, row );

   EXPECT_EQ( win.proc.record_remove_db( row ), 2016 );
   EXPECT_TRUE( server.changes.isEmpty() ) << server.changes.join( "\n" ).toStdString();
}

// ------------------------------------------------------------ removals

// Noise of another edit's model, recorded against the edit being removed,
// would keep the database from removing the edit: it goes first, and its
// row under that other model says so
TEST_F( TestUSDataDb, RemovingAnEditFirstRemovesNoiseOfAnotherEditsModel )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString cross  = US_Util::new_guid();
   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, runA + ".1A280.e2401021200_a1_2DSA_i01.model",
                            edit2, "22", "<model/>" };
   server.noises << FNoise{ "47", cross, "21", "34", model2, "ti_noise",
                            runA + ".1A280.e2401021200_a1_2DSA_i01.ti_noise", "<noise/>" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int crow = rowOf( model, cross );
   ASSERT_GE( crow, 0 );
   ASSERT_EQ( model.row_datadesc( model.parent_row( crow ) ).dataGUID.toStdString(),
              model2.toStdString() );        // shown under its own model

   win.select( itemOfRow( win.tw, rowOf( model, editG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 )
      << win.proc.lastError().toStdString();

   // the edit is asked for as it is; the older procedure refuses it, and
   // gives back what it took, so the noise goes first in a second try
   EXPECT_EQ( server.changes, QStringList() << "delete_editedData 21"
                              << "delete_noise 47" << "delete_editedData 21" );
   EXPECT_EQ( server.txlog, QStringList() << "begin" << "rollback"
                            << "begin" << "commit" );
   EXPECT_EQ( model.row_datadesc( crow ).recState, (int)US_DataModel::NOSTAT );
   EXPECT_NE( stateOf( model, model2 ) & US_DataModel::REC_DB, 0 );
   EXPECT_EQ( server.edits.size(), 1 );
}

// Noise of another investigator's model, recorded against the edit, is not
// listed, and an older delete_editedData removes the edit's models and
// noise before it fails on it.  The removal is one transaction: nothing is
// removed, the rows say so, and the message says why
TEST_F( TestUSDataDb, AnEditAnotherInvestigatorsNoiseKeepsLosesNothing )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   FModel theirs { "34", model2, "other.model", edit2, "22", "<model/>" };
   theirs.owner = kOther;
   server.models << theirs;
   server.noises << FNoise{ "48", US_Util::new_guid(), "21", "34", model2,
                            "ti_noise", "hidden.ti_noise", "<noise/>" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   ASSERT_EQ( rowOf( model, model2 ), -1 );           // not this investigator's
   int row = rowOf( model, editG );

   EXPECT_EQ( win.proc.record_remove_db( row ), 2012 );

   QString error = win.proc.lastError();
   EXPECT_TRUE( error.contains( "fk_noise_editDataID" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "another investigator" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "Nothing was removed" ) ) << error.toStdString();
   EXPECT_EQ( server.txlog, QStringList() << "begin" << "rollback" );

   // the database has everything still, and every row says it does
   EXPECT_EQ( server.edits.size(), 2 );
   EXPECT_NE( server.modelById( "31" ), nullptr );
   EXPECT_NE( server.noiseById( "41" ), nullptr );

   for ( const QString& guid : { editG, modelG, noiseG } )
   {
      Desc desc = model.row_datadesc( rowOf( model, guid ) );
      EXPECT_NE( desc.recState & US_DataModel::REC_DB, 0 ) << guid.toStdString();
      EXPECT_GE( desc.recordID, 0 );
   }
}

// Where the transaction cannot give back what an older delete_editedData
// removed -- the server ended it -- what is gone is read back, the rows of
// those records say so, and the message names them
TEST_F( TestUSDataDb, WhatARefusedEditRemovalTookIsReadBack )
{
   QString model2 = US_Util::new_guid();
   FModel theirs { "34", model2, "other.model", "", "1", "<model/>" };
   theirs.owner = kOther;
   server.models << theirs;
   server.noises << FNoise{ "48", US_Util::new_guid(), "21", "34", model2,
                            "ti_noise", "hidden.ti_noise", "<noise/>" };
   server.commitsInside << "delete_editedData";

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int row = rowOf( model, editG );
   EXPECT_EQ( win.proc.record_remove_db( row ), 2012 );

   QString error = win.proc.lastError();
   EXPECT_TRUE( error.contains( "removed from the database all the same" ) )
      << error.toStdString();
   EXPECT_TRUE( error.contains( model.row_datadesc( rowOf( model, modelG ) ).label ) )
      << error.toStdString();

   // the edit stayed, its model and noise did not
   EXPECT_NE( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );

   for ( const QString& guid : { modelG, noiseG } )
   {
      Desc desc = model.row_datadesc( rowOf( model, guid ) );
      EXPECT_EQ( desc.recState & US_DataModel::REC_DB, 0 ) << guid.toStdString();
      EXPECT_EQ( US_DataModel::sync_state( desc ), US_DataModel::SYNC_LOCAL );
   }
}

// The patched delete_editedData removes the noise of another edit's model
// recorded against the edit itself: nothing is removed first, and that
// noise's row, under its own model, says it is gone
TEST_F( TestUSDataDb, APatchedDatabaseTakesTheNoiseRecordedAgainstTheEdit )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString cross  = US_Util::new_guid();
   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, runA + ".1A280.e2401021200_a1_2DSA_i01.model",
                            edit2, "22", "<model/>" };
   server.noises << FNoise{ "47", cross, "21", "34", model2, "ti_noise",
                            runA + ".1A280.e2401021200_a1_2DSA_i01.ti_noise", "<noise/>" };
   server.patchedEditDelete = true;

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   win.select( itemOfRow( win.tw, rowOf( model, editG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 )
      << win.proc.lastError().toStdString();

   EXPECT_EQ( server.changes, QStringList() << "delete_editedData 21" );
   EXPECT_EQ( server.txlog, QStringList() << "begin" << "commit" );
   EXPECT_EQ( server.edits.size(), 1 );
   EXPECT_EQ( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
   EXPECT_EQ( stateOf( model, cross ), (int)US_DataModel::NOSTAT );
   EXPECT_NE( stateOf( model, model2 ) & US_DataModel::REC_DB, 0 );
}

// A patched delete_editedData that refuses the edit has changed nothing,
// and the second try, with the noise first, is rolled back as a whole
TEST_F( TestUSDataDb, APatchedRefusalRemovesNothing )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString cross  = US_Util::new_guid();
   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, runA + ".1A280.e2401021200_a1_2DSA_i01.model",
                            edit2, "22", "<model/>" };
   server.noises << FNoise{ "47", cross, "21", "34", model2, "ti_noise",
                            runA + ".1A280.e2401021200_a1_2DSA_i01.ti_noise", "<noise/>" };
   server.patchedEditDelete = true;
   server.refusing[ "delete_editedData" ] = 404;     // @CONSTRAINT_FAILED

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   EXPECT_EQ( win.proc.record_remove_db( rowOf( model, editG ) ), 2012 );
   QString error = win.proc.lastError();
   EXPECT_TRUE( error.contains( "status 404" ) ) << error.toStdString();
   EXPECT_TRUE( error.contains( "Nothing was removed" ) ) << error.toStdString();

   EXPECT_NE( server.noiseById( "47" ), nullptr );
   EXPECT_NE( server.modelById( "31" ), nullptr );
   for ( const QString& guid : { editG, modelG, noiseG, cross } )
      EXPECT_NE( stateOf( model, guid ) & US_DataModel::REC_DB, 0 ) << guid.toStdString();
}

// When the edit is still refused after the noise of another edit's model
// was removed first, and that removal was kept -- the transaction ended
// under it -- the message says so, and so does its row, under that model
TEST_F( TestUSDataDb, ForeignNoiseRemovedBeforeARefusedEditIsReported )
{
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString cross  = US_Util::new_guid();
   server.edits  << FEdit { "22", edit2, "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, runA + ".1A280.e2401021200_a1_2DSA_i01.model",
                            edit2, "22", "<model/>" };
   server.noises << FNoise{ "47", cross, "21", "34", model2, "ti_noise",
                            runA + ".1A280.e2401021200_a1_2DSA_i01.ti_noise", "<noise/>" };
   server.refusing[ "delete_editedData" ] = IUS_DB2::DBERROR;
   server.commitsInside << "delete_noise";

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int crow = rowOf( model, cross );
   ASSERT_GE( crow, 0 );

   EXPECT_EQ( win.proc.record_remove_db( rowOf( model, editG ) ), 2012 );
   EXPECT_EQ( server.changes, QStringList() << "delete_editedData 21"
                              << "delete_noise 47" << "delete_editedData 21" );

   QString error = win.proc.lastError();
   EXPECT_TRUE( error.contains( runA + ".1A280.e2401021200_a1_2DSA_i01.ti_noise" ) )
      << error.toStdString();
   EXPECT_TRUE( error.contains( "all the same" ) ) << error.toStdString();

   EXPECT_EQ( server.noiseById( "47" ), nullptr );
   EXPECT_EQ( model.row_datadesc( crow ).recState, (int)US_DataModel::NOSTAT );
   EXPECT_NE( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
   EXPECT_NE( stateOf( model, modelG ) & US_DataModel::REC_DB, 0 );
}

// The noise of another edit's model that cannot be listed stops nothing
// before the database is asked; noise that cannot be removed stops the
// edit, and nothing is removed
TEST_F( TestUSDataDb, AnEditTheServerKeepsIsReportedWithItsReason )
{
   server.edits  << FEdit { "22", US_Util::new_guid(), "11", runA + ".2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", US_Util::new_guid(), "other.model", "", "22", "" };
   server.noises << FNoise{ "48", US_Util::new_guid(), "21", "34", server.models.last().guid,
                            "ti_noise", "hidden.ti_noise", "" };
   server.denied << "get_noise_desc_by_editID";

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, editG );
   EXPECT_EQ( win.proc.record_remove_db( row ), 2017 );
   EXPECT_TRUE( win.proc.lastError().contains( "could not be listed" ) )
      << win.proc.lastError().toStdString();
   EXPECT_TRUE( server.changes.isEmpty() );

   server.denied.clear();
   server.refusing[ "delete_noise" ] = IUS_DB2::NOTPERMITTED;
   EXPECT_EQ( win.proc.record_remove_db( row ), 2018 );
   EXPECT_TRUE( win.proc.lastError().contains( "permission" ) )
      << win.proc.lastError().toStdString();
   EXPECT_TRUE( win.proc.lastError().contains( "Nothing was removed" ) )
      << win.proc.lastError().toStdString();
   EXPECT_EQ( server.changes, QStringList() << "delete_editedData 21"
                              << "delete_noise 48" );
   EXPECT_EQ( server.txlog, QStringList() << "begin" << "rollback"
                            << "begin" << "rollback" );
   EXPECT_NE( stateOf( model, editG ) & US_DataModel::REC_DB, 0 );
   EXPECT_NE( server.noiseById( "48" ), nullptr );
}

// Noise of another edit's model goes with the edit, so it is guarded as if
// it had been selected: by the model GMP made, and by the run it is of
TEST_F( TestUSDataDb, ForeignNoiseTheGmpFrameworkNeedsKeepsTheEdit )
{
   QString rawB   = US_Util::new_guid();
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   server.exps   << FExp  { "2", US_Util::new_guid(), "RunB" };
   server.raws   << FRaw  { "12", rawB, "RunB.RA.1.A.280.auc", "2" };
   server.edits  << FEdit { "22", edit2, "12", "RunB.2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, "RunB.1A280.e2401021200_a1_2DSA-GL_i01.model",
                            edit2, "22", "<model/>" };
   server.noises << FNoise{ "47", US_Util::new_guid(), "21", "34", model2, "ti_noise",
                            "RunB.1A280.e2401021200_a1_2DSA-GL_i01.ti_noise", "<noise/>" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   // the model the noise was fitted with was made by GMP
   server.autoflowModels << "34";
   win.select( itemOfRow( win.tw, rowOf( model, editG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_REFUSED );
   ASSERT_EQ( win.prompts.size(), 1 );
   EXPECT_TRUE( win.prompts[ 0 ].contains( "GMP" ) ) << win.prompts[ 0 ].toStdString();
   EXPECT_TRUE( server.changes.isEmpty() ) << server.changes.join( "\n" ).toStdString();

   // the run the noise is of is GMP's, though the edit's run is not
   server.autoflowModels.clear();
   server.autoflowRuns << "RunB";
   win.prompts.clear();
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ),
              US_DataTree::ACT_REFUSED );
   ASSERT_EQ( win.prompts.size(), 1 );
   EXPECT_TRUE( win.prompts[ 0 ].contains( "RunB" ) ) << win.prompts[ 0 ].toStdString();
   EXPECT_TRUE( server.changes.isEmpty() ) << server.changes.join( "\n" ).toStdString();
   EXPECT_NE( server.noiseById( "47" ), nullptr );
}

// The rows of another experiment an edit removal reached -- its noise of
// another edit's model is under that model -- are shown as they are now
TEST_F( TestUSDataDb, AnotherExperimentsRowsAnEditRemovalReachedAreRepainted )
{
   QString rawB   = US_Util::new_guid();
   QString edit2  = US_Util::new_guid();
   QString model2 = US_Util::new_guid();
   QString cross  = US_Util::new_guid();
   server.exps   << FExp  { "2", US_Util::new_guid(), "RunB" };
   server.raws   << FRaw  { "12", rawB, "RunB.RA.1.A.280.auc", "2" };
   server.edits  << FEdit { "22", edit2, "12", "RunB.2401021200.RA.1.A.280.xml" };
   server.models << FModel{ "34", model2, "RunB.1A280.e2401021200_a1_2DSA-GL_i01.model",
                            edit2, "22", "<model/>" };
   server.noises << FNoise{ "47", cross, "21", "34", model2, "ti_noise",
                            "RunB.1A280.e2401021200_a1_2DSA-GL_i01.ti_noise", "<noise/>" };

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int crow = rowOf( model, cross );
   ASSERT_GE( crow, 0 );
   ASSERT_EQ( model.run_of_row( crow ), model.index_of_run( "RunB" ) );
   QTreeWidgetItem* citem = itemOfRow( win.tw, crow );
   ASSERT_NE( citem, nullptr );
   ASSERT_EQ( citem->text( 3 ).toStdString(), "DB" );

   win.select( itemOfRow( win.tw, rowOf( model, editG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 0 )
      << win.proc.lastError().toStdString();

   EXPECT_EQ( model.row_datadesc( crow ).recState, (int)US_DataModel::NOSTAT );
   EXPECT_EQ( citem->text( 3 ).toStdString(), "dummy" );
   QTreeWidgetItem* mitem = itemOfRow( win.tw, rowOf( model, model2 ) );
   ASSERT_NE( mitem, nullptr );
   EXPECT_EQ( mitem->text( 4 ).toStdString(), "0, 0" );
}

// Another investigator's records are not listed, and are not removed when
// a row names them
TEST_F( TestUSDataDb, AnotherInvestigatorsRecordsAreNeitherListedNorRemoved )
{
   QString theirsG = US_Util::new_guid();
   FModel  theirs { "34", theirsG, runA + ".1A280.e2401011200_a9_2DSA_i01.model",
                    editG, "21", "<model/>" };
   theirs.owner = kOther;
   server.models << theirs;

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   EXPECT_EQ( rowOf( model, theirsG ), -1 );

   int  row  = rowOf( model, modelG );
   Desc desc = model.row_datadesc( row );
   desc.recordID = 34;                       // as a stale row might
   model.change_datadesc( desc, row );

   EXPECT_EQ( win.proc.record_remove_db( row ), 2013 );
   EXPECT_TRUE( win.proc.lastError().contains( "permission" ) )
      << win.proc.lastError().toStdString();
   EXPECT_NE( server.modelById( "34" ), nullptr );
   EXPECT_NE( stateOf( model, modelG ) & US_DataModel::REC_DB, 0 );
}

// A removal the server refuses says why, and the record stays where it is
TEST_F( TestUSDataDb, ARefusedRemovalSaysWhyAndKeepsTheRecord )
{
   server.refusing[ "delete_model" ] = IUS_DB2::NOTPERMITTED;

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   win.select( itemOfRow( win.tw, rowOf( model, modelG ) ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "DB-only remove" ), 2013 );
   EXPECT_TRUE( win.proc.lastError().contains( "permission" ) )
      << win.proc.lastError().toStdString();
   EXPECT_NE( stateOf( model, modelG ) & US_DataModel::REC_DB, 0 );
   EXPECT_EQ( server.models.size(), 1 );
}

// ------------------------------------------------------------ uploads

// A file changed since the comparison is what an upload compares, not the
// comparison made before it changed
TEST_F( TestUSDataDb, AFileChangedSinceTheComparisonIsUploaded )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, editG );
   ASSERT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC );

   QFile file( editPath );
   ASSERT_TRUE( file.open( QIODevice::Append ) );
   file.write( "\n" );
   file.close();

   EXPECT_EQ( win.proc.record_upload( row ), 0 ) << win.proc.lastError().toStdString();
   EXPECT_EQ( server.count( "update_editedData" ), 1 );
}

// ------------------------------------------------------------ downloads

// A model is written as the UTF-8 the database holds, whatever the locale
TEST_F( TestUSDataDb, AModelIsDownloadedAsUtf8 )
{
   QString newM = US_Util::new_guid();
   QString xml  = QString( "<?xml version=\"1.0\"?>\n<ModelData><model description=\"" )
                + QString::fromUtf8( "Ångström µ 2DSA" )
                + "\" modelGUID=\"" + newM + "\" editGUID=\"" + editG
                + "\"/></ModelData>\n";
   server.models << FModel{ "37", newM, runA + ".1A280.e2401011200_a3_2DSA_i01.model",
                            editG, "21", xml };
   server.digest[ newM ] = FakeServer::md5Of( xml );

#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
   // A locale that is not UTF-8 is what lost these characters
   QTextCodec* saved = QTextCodec::codecForLocale();
   QTextCodec::setCodecForLocale( QTextCodec::codecForName( "ISO-8859-1" ) );
#endif

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, newM );
   ASSERT_GE( row, 0 );
   int stat = win.proc.record_download( row );

#if QT_VERSION < QT_VERSION_CHECK( 6, 0, 0 )
   QTextCodec::setCodecForLocale( saved );
#endif

   ASSERT_EQ( stat, 0 ) << win.proc.lastError().toStdString();
   QString path = model.row_datadesc( row ).filename;
   QFile   file( path );
   ASSERT_TRUE( file.open( QIODevice::ReadOnly ) );
   EXPECT_EQ( file.readAll(), xml.toUtf8() );
}

// ------------------------------------------------------------ reads

// A side that cannot read an experiment is not merged as if it held
// nothing: the local records would read as local only.  A lost connection
// is not tried again -- US_DB2 does not connect again -- and the message
// says what will
TEST_F( TestUSDataDb, AnExperimentADatabaseReadFailedOnIsNotShownAsLocal )
{
   server.failing << "get_rawData_by_experiment";

   US_DataModel model;
   prepare( model, runA );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 1 );
   EXPECT_FALSE( model.run_entry( 0 ).loaded );
   EXPECT_EQ( rowOf( model, editG ), -1 );
   ASSERT_FALSE( model.scan_errors().isEmpty() );
   EXPECT_TRUE( model.scan_errors().join( "\n" ).contains( "connection to the database was lost" ) )
      << model.scan_errors().join( "\n" ).toStdString();
   EXPECT_EQ( server.count( "get_rawData_by_experiment" ), 1 );   // not again

   // opening it later tries again
   server.failing.clear();
   EXPECT_TRUE( model.scan_run( 0 ) );
   EXPECT_TRUE( model.run_entry( 0 ).loaded );
   EXPECT_EQ( stateOf( model, editG ) & ( US_DataModel::REC_DB | US_DataModel::REC_LO ),
              US_DataModel::REC_DB | US_DataModel::REC_LO );
}

// A read that fails once, on something that passes -- a lock the server
// waited on too long -- is tried again before anything is given up
TEST_F( TestUSDataDb, AReadThatFailsOnceIsTriedAgain )
{
   server.transient[ "get_rawData_by_experiment" ] = 1;

   US_DataModel model;
   prepare( model, runA );
   model.scan_data();

   EXPECT_TRUE( model.run_entry( 0 ).loaded );
   EXPECT_TRUE( model.scan_errors().isEmpty() ) << model.scan_errors().join( "\n" ).toStdString();
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, editG ) ) ),
              US_DataModel::SYNC_INSYNC );
}

// Read one experiment at a time, noise recorded against a later
// experiment's edit for a model of an experiment shown already goes under
// that model: the catalog says so, and that experiment is merged again
TEST_F( TestUSDataDb, NoiseReadWithALaterExperimentJoinsItsModel )
{
   QString cross = US_Util::new_guid();
   server.exps   << FExp  { "2", US_Util::new_guid(), "RunB" };
   server.raws   << FRaw  { "12", US_Util::new_guid(), "RunB.RA.1.A.280.auc", "2" };
   server.edits  << FEdit { "22", US_Util::new_guid(), "12", "RunB.2401011200.RA.1.A.280.xml" };
   server.noises << FNoise{ "63", cross, "22", "31", modelG, "ri_noise",
                            runA + ".1A280.e2401011200_a1_2DSA_i01.ri_noise", "<noise/>" };
   server.failing << "get_rawData_by_person";        // the whole-store read fails

   US_DataModel model;
   prepare( model );
   Window       win( model );

   QList< int > reloaded;
   QObject::connect( &model, &US_DataModel::runsListed,
                     [ & ]() { win.tree.build_dtree(); } );
   QObject::connect( &model, &US_DataModel::runMerged,
                     [ & ]( int index ) { win.tree.fill_run( index ); } );
   QObject::connect( &model, &US_DataModel::runReloaded, [ & ]( int index )
                     { reloaded << index; win.tree.refill_run( index ); } );

   model.scan_data();

   int runa = model.index_of_run( runA );
   ASSERT_EQ( reloaded, QList< int >() << runa );

   int row = rowOf( model, cross );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( model.run_of_row( row ), runa );
   EXPECT_EQ( model.row_datadesc( model.parent_row( row ) ).dataGUID.toStdString(),
              modelG.toStdString() );

   // one row each, the rows left behind are no one's, and the tree and the
   // counts show the new rows
   int nrows = 0;
   for ( int rr = 0; rr < model.recCount(); rr++ )
      nrows += ( model.row_datadesc( rr ).dataGUID == modelG  &&
                 model.run_of_row( rr ) >= 0 ) ? 1 : 0;
   EXPECT_EQ( nrows, 1 );
   EXPECT_TRUE( model.find_duplicates().isEmpty() );
   EXPECT_NE( itemOfRow( win.tw, row ), nullptr );
   QTreeWidgetItem* mitem = itemOfRow( win.tw, rowOf( model, modelG, runa ) );
   ASSERT_NE( mitem, nullptr );
   EXPECT_EQ( mitem->text( 4 ).toStdString(), "2, 1" );
   EXPECT_EQ( model.count_records().db[ US_DataModel::NOISE ], 2 );

   // and it is compared again: the local copies are still in sync
   EXPECT_TRUE( model.run_entry( runa ).verified );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, editG, runa ) ) ),
              US_DataModel::SYNC_INSYNC );
}

// The run ID finds an experiment as the catalogs find one: exactly first,
// then without case
TEST_F( TestUSDataDb, AnExperimentIsFoundByRunIdAsTheCatalogsFindIt )
{
   server.exps << FExp{ "2", US_Util::new_guid(), "runa" };

   US_DataModel model;
   prepare( model, "ALL", "DB Only" );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 2 );
   EXPECT_EQ( model.run_entry( model.index_of_run( "runa" ) ).runID.toStdString(), "runa" );
   EXPECT_EQ( model.run_entry( model.index_of_run( runA ) ).runID.toStdString(),
              runA.toStdString() );
   EXPECT_GE( model.index_of_run( "RUNA" ), 0 );
   EXPECT_EQ( model.index_of_run( "RunZ" ), -1 );
}

// ------------------------------------------------------------ pairing

// A local run pairs with the database experiment its experiment file
// names, not with the first one of its run ID
TEST_F( TestUSDataDb, ALocalRunPairsWithTheExperimentItRecords )
{
   QString exp2 = US_Util::new_guid();
   server.exps << FExp{ "2", exp2, runA };

   QFile file( runDir( runA ) + "/" + runA + ".RA.xml" );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly | QIODevice::Text ) );
   file.write( QString( "<?xml version=\"1.0\"?>\n<experiment id=\"2\" guid=\"%1\""
                        " type=\"velocity\">\n</experiment>\n" ).arg( exp2 ).toUtf8() );
   file.close();

   US_DataModel model;
   prepare( model );
   model.scan_data();

   ASSERT_EQ( model.runCount(), 2 );
   int both = -1;
   for ( int ii = 0; ii < 2; ii++ )
      if ( model.run_entry( ii ).inBoth() )  both = ii;
   ASSERT_GE( both, 0 );
   EXPECT_EQ( model.row_datadesc( model.run_entry( both ).row ).recordID, 2 );
}

// A global model one source gives no edit GUID and the other the all-zero
// one is one record in both places
TEST_F( TestUSDataDb, AGlobalModelInBothPlacesIsOneRecord )
{
   QString glob  = US_Util::new_guid();
   QString gdesc = "Global.global.e1_a1_2DSA-GL_i01.model";
   QString mpath = US_Settings::dataDir() + "/models/M0000008.xml";
   writeModel( mpath, glob, "00000000-0000-0000-0000-000000000000", gdesc );
   server.models << FModel{ "38", glob, gdesc, "", "", textOf( mpath ) };
   server.digest[ glob ] = US_Util::md5sum_file( mpath );

   US_DataModel model;
   prepare( model );
   model.scan_data();

   int nrows = 0;
   for ( int rr = 0; rr < model.recCount(); rr++ )
      nrows += ( model.row_datadesc( rr ).dataGUID == glob ) ? 1 : 0;
   EXPECT_EQ( nrows, 1 );

   int row = rowOf( model, glob );
   EXPECT_EQ( model.run_of_row( row ), standIn( model, true ) );
   EXPECT_EQ( model.row_datadesc( row ).recState & ( US_DataModel::REC_DB | US_DataModel::REC_LO ),
              US_DataModel::REC_DB | US_DataModel::REC_LO );
}

// ------------------------------------------------------------ filters

// The records of no experiment follow the source filter, as any others do
TEST_F( TestUSDataDb, OrphansFollowTheExcludeFilters )
{
   QString lost = US_Util::new_guid();
   server.models << FModel{ "32", lost, runA + ".1A280.lost.model",
                            US_Util::new_guid(), "99", "" };

   {
      US_DataModel model;
      prepare( model, "ALL", "Exclude DB-Only Trees" );
      model.scan_data();

      EXPECT_EQ( rowOf( model, lost ), -1 );
      EXPECT_LT( standIn( model, false ), 0 );
      EXPECT_GE( rowOf( model, editG ), 0 );         // in both: kept
   }

   {
      US_DataModel model;
      prepare( model, "ALL", "Exclude Local-Only Trees" );
      model.scan_data();

      int orph = standIn( model, false );
      ASSERT_GE( orph, 0 );
      EXPECT_EQ( model.run_of_row( rowOf( model, lost ) ), orph );
   }
}

// ------------------------------------------------------------ bookkeeping

// An action on many records of one experiment brings it up to date once
TEST_F( TestUSDataDb, AnActionOnManyRecordsAsksForOneComparison )
{
   for ( int ii = 3; ii <= 6; ii++ )
   {
      QString guid = US_Util::new_guid();
      QString tmp  = root + QString( "/blob%1.xml" ).arg( ii );
      writeEdit( tmp, guid, rawG );
      server.edits << FEdit{ QString::number( 20 + ii ), guid, "11",
                             runA + QString( ".240103120%1.RA.1.A.280.xml" ).arg( ii ) };
      server.blobs[ 20 + ii ] = textOf( tmp ).toUtf8();
      server.digest[ guid ]   = US_Util::md5sum_file( tmp );
   }

   server.blobs[ 21 ] = textOf( editPath ).toUtf8();   // the one in both

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();
   win.tree.build_dtree();

   int asked = 0;
   QObject::connect( &model, &US_DataModel::workRequested, [ & ]() { asked++; } );

   win.select( win.tw.topLevelItem( 0 ) );
   EXPECT_EQ( win.tree.act_on_selection( "Database only", "Local file create" ), 0 )
      << win.proc.lastError().toStdString();
   EXPECT_EQ( asked, 1 );
   EXPECT_EQ( model.run_entry( 0 ).loCount, 8 );
   EXPECT_EQ( model.pending_verifies(), 1 );
}

// ------------------------------------------------------------ merged again

// RunA is merged, and RunB not yet: noise RunB's edit holds for RunA's
// model will merge RunA again when RunB is read
class TestUSDataDbRemerge : public TestUSDataDb
{
   protected:
      QString cross;
      QString dbOnly;                     // a model in the database only
      QString loOnly;                     // a model on local disk only
      int     runa = -1;
      int     runb = -1;

      void SetUp() override
      {
         TestUSDataDb::SetUp();

         cross  = US_Util::new_guid();
         dbOnly = US_Util::new_guid();
         loOnly = US_Util::new_guid();

         server.exps   << FExp  { "2", US_Util::new_guid(), "RunB" };
         server.raws   << FRaw  { "12", US_Util::new_guid(), "RunB.RA.1.A.280.auc", "2" };
         server.edits  << FEdit { "22", US_Util::new_guid(), "12", "RunB.2401011200.RA.1.A.280.xml" };
         server.noises << FNoise{ "63", cross, "22", "31", modelG, "ri_noise",
                                  runA + ".1A280.e2401011200_a1_2DSA_i01.ri_noise", "<noise/>" };

         QString tmp = root + "/dbonly.xml";
         writeModel( tmp, dbOnly, editG, runA + ".1A280.e2401011200_a5_2DSA_i01.model" );
         server.models << FModel{ "35", dbOnly, runA + ".1A280.e2401011200_a5_2DSA_i01.model",
                                  editG, "21", textOf( tmp ) };
         server.digest[ dbOnly ] = FakeServer::md5Of( textOf( tmp ) );

         writeModel( US_Settings::dataDir() + "/models/M0000005.xml", loOnly, editG,
                     runA + ".1A280.e2401011200_a6_2DSA_i01.model" );

         server.failing << "get_rawData_by_person";    // one experiment at a time
      }

      // Scan as far as RunA, and show it; the model is prepared
      void scanRunA( US_DataModel& model, Window& win )
      {
         QObject::connect( &model, &US_DataModel::runMerged, [ & ]( int index )
         {
            if ( model.run_entry( index ).runID == runA )
               model.stop_scan();
         } );
         QObject::connect( &model, &US_DataModel::runReloaded,
                           [ & ]( int index ) { win.tree.refill_run( index ); } );

         model.scan_data();
         win.tree.build_dtree();

         runa = model.index_of_run( runA );
         runb = model.index_of_run( "RunB" );
         ASSERT_GE( runa, 0 );
         ASSERT_GE( runb, 0 );
         ASSERT_TRUE ( model.run_entry( runa ).loaded );
         ASSERT_FALSE( model.run_entry( runb ).loaded );
      }
};

// An experiment merged again from the catalogs shows what the actions since
// the scan did: a removed record stays removed, a downloaded one local, an
// uploaded one in the database -- and the two copies are compared again
TEST_F( TestUSDataDbRemerge, AMergeAgainKeepsWhatActionsDid )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   scanRunA( model, win );

   EXPECT_EQ( win.proc.record_remove_db( rowOf( model, noiseG, runa ) ), 0 )
      << win.proc.lastError().toStdString();
   EXPECT_EQ( win.proc.record_download( rowOf( model, dbOnly, runa ) ), 0 )
      << win.proc.lastError().toStdString();
   EXPECT_EQ( win.proc.record_upload( rowOf( model, loOnly, runa ) ), 0 )
      << win.proc.lastError().toStdString();

   QString loID;
   for ( const FModel& mm : server.models )  if ( mm.guid == loOnly )  loID = mm.id;
   ASSERT_FALSE( loID.isEmpty() );

   int reloaded = 0;
   QObject::connect( &model, &US_DataModel::runReloaded,
                     [ & ]( int index ) { reloaded += ( index == runa ) ? 1 : 0; } );

   EXPECT_TRUE( model.scan_run( runb ) );
   ASSERT_EQ( reloaded, 1 );

   Desc noise = model.row_datadesc( rowOf( model, noiseG, runa ) );
   EXPECT_EQ( noise.recState & US_DataModel::REC_DB, 0 );
   EXPECT_NE( noise.recState & US_DataModel::REC_LO, 0 );
   EXPECT_LT( noise.recordID, 0 );

   Desc down = model.row_datadesc( rowOf( model, dbOnly, runa ) );
   EXPECT_NE( down.recState & US_DataModel::REC_LO, 0 );
   EXPECT_NE( down.recState & US_DataModel::REC_DB, 0 );
   EXPECT_TRUE( QFile::exists( down.filename ) ) << down.filename.toStdString();

   Desc up = model.row_datadesc( rowOf( model, loOnly, runa ) );
   EXPECT_NE( up.recState & US_DataModel::REC_DB, 0 );
   EXPECT_EQ( up.recordID, loID.toInt() );

   // one row each in RunA, and the tree shows them so
   int ndown = 0;
   for ( int rr = 0; rr < model.recCount(); rr++ )
      ndown += ( model.row_datadesc( rr ).dataGUID == dbOnly  &&
                 model.run_of_row( rr ) == runa ) ? 1 : 0;
   EXPECT_EQ( ndown, 1 );
   QTreeWidgetItem* nitem = itemOfRow( win.tw, rowOf( model, noiseG, runa ) );
   ASSERT_NE( nitem, nullptr );
   EXPECT_EQ( nitem->text( 3 ).toStdString(), "Local" );

   // and a second download does not write another copy
   server.changes.clear();
   int before = QDir( US_Settings::dataDir() + "/models" ).entryList( QDir::Files ).size();
   EXPECT_EQ( win.proc.record_download( rowOf( model, dbOnly, runa ) ), 0 );
   EXPECT_EQ( QDir( US_Settings::dataDir() + "/models" ).entryList( QDir::Files ).size(), before );

   // the changed records are compared on their own, and found the same
   while ( model.pump_once() ) {}
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, dbOnly, runa ) ) ),
              US_DataModel::SYNC_INSYNC ) << model.scan_errors().join( "\n" ).toStdString();
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( rowOf( model, loOnly, runa ) ) ),
              US_DataModel::SYNC_INSYNC ) << model.scan_errors().join( "\n" ).toStdString();
}

// ------------------------------------------------------------ context menu

// Reading the experiments a context menu acts on can merge another one
// again: the item the menu is for, and the selection, are its new items
TEST_F( TestUSDataDbRemerge, AContextMenuTakesTheItemsAReadLeft )
{
   US_DataModel model;
   prepare( model );
   Window       win( model );
   scanRunA( model, win );

   int              nrow  = rowOf( model, noiseG, runa );
   QTreeWidgetItem* nitem = itemOfRow( win.tw, nrow );
   ASSERT_NE( nitem, nullptr );

   win.tw.clearSelection();
   nitem->setSelected( true );
   win.tw.topLevelItem( runb )->setSelected( true );

   // reading RunB merges RunA again: the noise item goes
   ASSERT_TRUE( win.tree.context_selection( nitem ) );
   EXPECT_TRUE( model.run_entry( runb ).loaded );

   QTreeWidgetItem* cur = win.tree.current_item();
   ASSERT_NE( cur, nullptr );
   int crow = cur->type() - (int)QTreeWidgetItem::UserType;
   EXPECT_NE( crow, nrow );
   EXPECT_EQ( model.run_of_row( crow ), runa );
   EXPECT_EQ( model.row_datadesc( crow ).dataGUID.toStdString(), noiseG.toStdString() );

   QList< QTreeWidgetItem* > sel = win.tw.selectedItems();
   EXPECT_TRUE( sel.contains( cur ) );
   EXPECT_TRUE( sel.contains( win.tw.topLevelItem( runb ) ) );
   EXPECT_EQ( sel.size(), 2 );

   // and the action rows are RunB's records and the new noise row
   QVector< int > rows = win.tree.action_row_list();
   EXPECT_TRUE( rows.contains( crow ) );
   EXPECT_FALSE( rows.contains( nrow ) );
}

// ------------------------------------------------------------ line ends

// A model file with CRLF line ends -- as written on Windows -- holds what the
// database holds, with LF ones: it is in sync, and nothing is uploaded
TEST_F( TestUSDataDb, AModelFileWithCrlfLineEndsIsInSync )
{
   QByteArray bytes = textOf( modelPath ).toUtf8();
   ASSERT_FALSE( bytes.contains( '\r' ) );
   bytes.replace( "\n", "\r\n" );
   QFile file( modelPath );
   ASSERT_TRUE( file.open( QIODevice::WriteOnly ) );
   file.write( bytes );
   file.close();

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, modelG );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( US_DataModel::sync_state( model.row_datadesc( row ) ),
              US_DataModel::SYNC_INSYNC ) << model.row_datadesc( row ).contents.toStdString();

   Desc desc = model.row_datadesc( row );       // not compared since
   desc.recState &= ~US_DataModel::HV_DET;
   desc.contents  = QString();
   model.change_datadesc( desc, row );
   EXPECT_EQ( win.proc.record_upload( row ), (int)US_DataProcess::IN_SYNC );
   EXPECT_EQ( server.count( "update_model" ), 0 );
}

// ------------------------------------------------------------ record IDs

// A model written whose ID cannot be read back is in the database all the
// same: its row says so, with no ID to act on it by until a new scan
TEST_F( TestUSDataDb, AWrittenModelWithoutAnIdIsInTheDatabase )
{
   QString newM = US_Util::new_guid();
   writeModel( US_Settings::dataDir() + "/models/M0000002.xml", newM, editG,
               runA + ".1A280.e2401011200_a2_2DSA_i01.model" );

   US_DataModel model;
   prepare( model );
   Window       win( model );
   model.scan_data();

   int row = rowOf( model, newM );
   ASSERT_GE( row, 0 );

   server.failAfter[ "get_modelID" ] = server.count( "get_modelID" ) + 1;
   EXPECT_EQ( win.proc.record_upload( row ), 3047 );
   EXPECT_TRUE( win.proc.lastError().contains( "scan again" ) )
      << win.proc.lastError().toStdString();

   Desc desc = model.row_datadesc( row );
   EXPECT_NE( desc.recState & US_DataModel::REC_DB, 0 );
   EXPECT_LT( desc.recordID, 0 );
   EXPECT_EQ( win.proc.record_remove_db( row ), 2016 );   // nothing to go by
}

// A patched server lists noise with the experiment of its model: noise
// recorded against a later experiment's edit is read with the model's own
// experiment, under its model, once, and nothing is merged again
TEST_F( TestUSDataDb, NoiseListedWithItsModelsExperimentIsShownOnce )
{
   QString cross = US_Util::new_guid();
   server.exps   << FExp  { "2", US_Util::new_guid(), "RunB" };
   server.raws   << FRaw  { "12", US_Util::new_guid(), "RunB.RA.1.A.280.auc", "2" };
   server.edits  << FEdit { "22", US_Util::new_guid(), "12", "RunB.2401011200.RA.1.A.280.xml" };
   server.noises << FNoise{ "63", cross, "22", "31", modelG, "ri_noise",
                            runA + ".1A280.e2401011200_a1_2DSA_i01.ri_noise", "<noise/>" };
   server.noiseByModel = true;
   server.failing << "get_rawData_by_person";        // one experiment at a time

   US_DataModel model;
   prepare( model );

   int reloaded = 0;
   QObject::connect( &model, &US_DataModel::runReloaded, [ & ]( int ) { reloaded++; } );

   model.scan_data();

   int runa = model.index_of_run( runA );
   int row  = rowOf( model, cross );
   ASSERT_GE( row, 0 );
   EXPECT_EQ( model.run_of_row( row ), runa );
   EXPECT_EQ( model.row_datadesc( model.parent_row( row ) ).dataGUID.toStdString(),
              modelG.toStdString() );
   EXPECT_EQ( reloaded, 0 );

   int nrows = 0;
   for ( int rr = 0; rr < model.recCount(); rr++ )
      nrows += ( model.row_datadesc( rr ).dataGUID == cross  &&
                 model.run_of_row( rr ) >= 0 ) ? 1 : 0;
   EXPECT_EQ( nrows, 1 );
   EXPECT_EQ( model.count_records().db[ US_DataModel::NOISE ], 2 );
   EXPECT_EQ( model.run_entry( runa ).dbCount, model.count_records( runa ).db[ 1 ]
                                               + model.count_records( runa ).db[ 2 ]
                                               + model.count_records( runa ).db[ 3 ]
                                               + model.count_records( runa ).db[ 4 ] );
}
