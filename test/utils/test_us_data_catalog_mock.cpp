// test_us_data_catalog_mock.cpp - US_DataCatalog's database paths, against
// a mock connection standing in for servers of every vintage
//
// The mock answers the stored procedures from one small data set, the way
// a server would: every procedure that exists gives the same records, in
// its own column order.  A test then takes procedures away -- all of a
// tier, or just one, the way a partly upgraded server lacks them -- and
// checks that the catalog falls back and still reads the same chain.
//
// The column layouts and the scoping are the server's, from the us3_sql
// repository: the catalog procedures in us3_catalog_procs.sql, the older
// ones in us3_exp_procs.sql, us3_expdata_procs.sql, us3_model_procs.sql and
// us3_noise_procs.sql, the column widths in us3.sql.  In particular every
// model and noise listing joins modelPerson: a model is listed only to the
// investigator it belongs to, and noise only with such a model, by
// noise.modelID.
#include "mock_us_db2.h"
#include "qt_test_base.h"

#include <QtCore>
#include <QSignalSpy>

#include <ctime>

#include "us_data_catalog.h"
#include "us_settings.h"

using ::testing::_;
using ::testing::An;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

namespace
{
   const int     kInv  = 5;
   const QString kDate = "2024-03-04 05:06:07";
   const QString kUtc  = "2024-03-04 05:06:07 UTC";

   // An experiment belongs to one investigator (experimentPerson), and so
   // does a model (modelPerson); a description longer than the server's
   // column comes back cut from the listings and whole from the record's XML
   struct FExp   { QString id, guid, runID, projID, projGUID, projDesc; int inv = kInv; };
   struct FRaw   { QString id, guid, file, expID, comment, solID; };
   struct FEdit  { QString id, guid, rawID, file; };
   struct FModel { QString id, guid, desc, editGUID, editID; int inv = kInv; };
   struct FNoise { QString id, guid, editID, modelID, modelGUID, type, desc; };

   // model.description and noise.description are VARCHAR(160) (us3.sql)
   const int kDescWidth = 160;

   // What one procedure call answers
   struct Answer
   {
      int                   status = IUS_DB2::OK;
      int                   server = 0;
      QString               error;
      QList< QVariantList > rows;
   };

   QString cksum( const QString& guid ) { return "md5-" + guid; }

   /* A server holding one investigator's store, answering the US3 stored
      procedures from it */
   class FakeServer
   {
      public:
         QList< FExp >   exps;
         QList< FRaw >   raws;
         QList< FEdit >  edits;
         QList< FModel > models;
         QList< FNoise > noises;

         QSet< QString >       missing;   // answer ER_SP_DOES_NOT_EXIST
         QSet< QString >       denied;    // answer ER_PROCACCESS_DENIED
         QSet< QString >       refused;   // answer NOTPERMITTED, as for a
                                          //  record of another investigator
         int                   caller = kInv;   // who is connected

         // How get_experiment_summary counts noise: by the experiment of its
         // model, as the catalog files it (the current procedure), or by the
         // edit it was recorded against (the first version)
         bool                  countNoiseByEdit = false;
         QMap< QString, int >  failOn;    // fail the n-th call; 0 = every call
         QMap< QString, int >  knt;       // calls so far, by procedure
         QStringList           calls;     // procedure names, in order

         Answer current;
         int    row = -1;

         void wire( NiceMock< US_DB2_Mock >& db )
         {
            ON_CALL( db, isConnected() ).WillByDefault( Return( true ) );
            ON_CALL( db, query( An< const QStringList& >() ) )
               .WillByDefault( Invoke( [ this ]( const QStringList& q ) { call( q ); } ) );
            ON_CALL( db, next() )
               .WillByDefault( Invoke( [ this ]() { return ++row < current.rows.size(); } ) );
            ON_CALL( db, value( _ ) )
               .WillByDefault( Invoke( [ this ]( unsigned ii ) -> QVariant
               {
                  if ( row < 0  ||  row >= current.rows.size() )  return QVariant();
                  return current.rows[ row ].value( int( ii ) );
               } ) );
            ON_CALL( db, lastErrno() )
               .WillByDefault( Invoke( [ this ]() { return current.status; } ) );
            ON_CALL( db, lastError() )
               .WillByDefault( Invoke( [ this ]() { return current.error; } ) );
            ON_CALL( db, lastServerErrno() )
               .WillByDefault( Invoke( [ this ]() { return current.server; } ) );
         }

         int count( const QString& name ) const { return calls.count( name ); }

         // ------------------------------------------------ the procedures
         void call( const QStringList& q )
         {
            const QString name = q.value( 0 );
            calls << name;
            int nth = ++knt[ name ];

            current = Answer();
            row     = -1;

            if ( missing.contains( name ) )
            {
               current.status = IUS_DB2::DBERROR;
               current.server = IUS_DB2::serverNoSuchRoutine;
               current.error  = "MySQL error: PROCEDURE us3." + name + " does not exist";
               return;
            }

            if ( denied.contains( name ) )
            {
               current.status = IUS_DB2::DBERROR;
               current.server = IUS_DB2::serverRoutineDenied;
               current.error  = "MySQL error: execute command denied for " + name;
               return;
            }

            if ( failOn.contains( name )  &&
                 ( failOn[ name ] == 0  ||  failOn[ name ] == nth ) )
            {
               current.status = IUS_DB2::DBERROR;
               current.server = 2013;            // lost connection
               current.error  = "MySQL error: Lost connection during " + name;
               return;
            }

            if ( refused.contains( name ) )
            {
               current.status = IUS_DB2::NOTPERMITTED;
               current.error  = "You do not have permission for " + name;
               return;
            }

            // An experiment is described only to the investigator it is of
            if ( name == "get_experiment_info" )
            {
               const FExp* ee = expById( q.value( 1 ) );

               if ( ee != nullptr  &&  ee->inv != caller )
               {
                  current.status = IUS_DB2::NOTPERMITTED;
                  current.error  = "not your experiment";
                  return;
               }
            }

            current.rows = rowsOf( name, q.mid( 1 ) );

            // Most procedures answer NOROWS for nothing; these answer OK
            // with an empty set, as us3_catalog_procs.sql writes them
            static const QSet< QString > emptyIsOk{ "get_experiment_summary",
                                                     "get_model_desc_by_experiment",
                                                     "get_noise_desc_by_experiment",
                                                     "get_rawData_by_person",
                                                     "get_editedData_by_person" };

            if ( current.rows.isEmpty()  &&  ! emptyIsOk.contains( name ) )
            {
               current.status = IUS_DB2::NOROWS;
               current.error  = "no rows";
            }
         }

         const FExp* expById( const QString& id ) const
         {
            for ( const FExp& ee : exps )  if ( ee.id == id )  return &ee;
            return nullptr;
         }

         const FRaw* rawById( const QString& id ) const
         {
            for ( const FRaw& rr : raws )  if ( rr.id == id )  return &rr;
            return nullptr;
         }

         QString expOfEdit( const QString& editID ) const
         {
            for ( const FEdit& ed : edits )
               if ( ed.id == editID )
                  return rawById( ed.rawID ) ? rawById( ed.rawID )->expID : QString();
            return QString();
         }

         QString expOfModel( const QString& modelID ) const
         {
            for ( const FModel& mm : models )
               if ( ! mm.id.isEmpty()  &&  mm.id == modelID )
                  return expOfEdit( mm.editID );
            return QString();
         }

         QString rawGUIDOf( const FEdit& ed ) const
         {
            return rawById( ed.rawID ) ? rawById( ed.rawID )->guid : QString( "orphan-raw" );
         }

         QVariantList rawRow( const FRaw& rr, bool hashed ) const
         {  // get_rawData_by_experiment / _by_person (us3_catalog_procs.sql):
            // rawDataID, rawDataGUID, label, filename, comment, experimentID,
            // solutionID, channelID, UTC_lastUpdated, checksum, size; the
            // _by_person form hashes nothing and gives NULL for the last two
            return QVariantList() << rr.id << rr.guid << "label" << "C:\\data\\" + rr.file
                                  << rr.comment << rr.expID << rr.solID << "1" << kDate
                                  << ( hashed ? cksum( rr.guid ) : QString() )
                                  << ( hashed ? "100" : "" );
         }

         QVariantList editRow( const FEdit& ed, bool hashed ) const
         {  // get_editedData_by_experiment / _by_person (us3_catalog_procs.sql):
            // editedDataID, rawDataID, rawDataGUID, editGUID, label, filename,
            // comment, UTC_lastUpdated, checksum, size
            return QVariantList() << ed.id << ed.rawID << rawGUIDOf( ed ) << ed.guid
                                  << "label" << ed.file << "comment" << kDate
                                  << ( hashed ? cksum( ed.guid ) : QString() )
                                  << ( hashed ? "200" : "" );
         }

         // Whether an experiment is of the investigator a listing is for
         bool mine( const QString& expID, const QString& inv ) const
         {
            const FExp* ee = expById( expID );
            return ee == nullptr  ||  QString::number( ee->inv ) == inv;
         }

         // Whether a model is of the investigator a listing is for: the
         // modelPerson join every model and noise listing makes
         bool modelOf( const QString& modelID, const QString& inv ) const
         {
            for ( const FModel& mm : models )
               if ( ! mm.id.isEmpty()  &&  mm.id == modelID )
                  return QString::number( mm.inv ) == inv;
            return false;
         }

         QVariantList modelRow( const FModel& mm ) const
         {  // get_model_desc, _by_editID (us3_model_procs.sql) and
            // get_model_desc_by_experiment (us3_catalog_procs.sql):
            // modelID, modelGUID, description, variance, meniscus, editGUID,
            // editedDataID, UTC_lastUpdated, checksum, size
            return QVariantList() << mm.id << mm.guid << mm.desc.left( kDescWidth ) << "" << ""
                                  << mm.editGUID << mm.editID << kDate
                                  << cksum( mm.guid ) << "300";
         }

         QVariantList noiseRow( const FNoise& nn ) const
         {  // get_noise_desc, _by_editID (us3_noise_procs.sql) and
            // get_noise_desc_by_experiment (us3_catalog_procs.sql):
            // noiseID, noiseGUID, editedDataID, modelID, noiseType, modelGUID,
            // UTC_timeEntered, checksum, size, description
            return QVariantList() << nn.id << nn.guid << nn.editID << nn.modelID
                                  << nn.type << nn.modelGUID << kDate
                                  << cksum( nn.guid ) << "400" << nn.desc.left( kDescWidth );
         }

         QList< QVariantList > rowsOf( const QString& name, const QStringList& a ) const
         {
            QList< QVariantList > rows;

            if ( name == "get_experiment_summary" )
               // us3_catalog_procs.sql: experimentID, experimentGUID, runID,
               // type, runType, label, projectID, projectGUID,
               // projectDescription, UTC_dateUpdated, rawCount, editCount,
               // modelCount, noiseCount.  The model and noise counts are of
               // the investigator's models (modelPerson), as the detail
               // procedures list them.
               for ( const FExp& ee : exps )
               {
                  if ( QString::number( ee.inv ) != a.value( 0 ) )  continue;

                  int nr = 0, ne = 0, nm = 0, nn = 0;
                  for ( const FRaw& rr : raws )   if ( rr.expID == ee.id )  nr++;
                  for ( const FEdit& ed : edits ) if ( rawById( ed.rawID ) && rawById( ed.rawID )->expID == ee.id ) ne++;
                  for ( const FModel& mm : models ) if ( expOfEdit( mm.editID ) == ee.id  &&  modelOf( mm.id, a.value( 0 ) ) ) nm++;
                  for ( const FNoise& no : noises )
                     if ( ( countNoiseByEdit ? expOfEdit( no.editID ) : expOfModel( no.modelID ) ) == ee.id  &&
                          modelOf( no.modelID, a.value( 0 ) ) )
                        nn++;
                  rows << ( QVariantList() << ee.id << ee.guid << ee.runID << "velocity" << "RA"
                                           << ee.runID + " label" << ee.projID << ee.projGUID
                                           << ee.projDesc << kDate << nr << ne << nm << nn );
               }

            else if ( name == "get_experiment_desc" )
            {  // what an investigator who is not an administrator is given
               for ( const FExp& ee : exps )
                  if ( QString::number( ee.inv ) == a.value( 0 ) )
                     rows << ( QVariantList() << ee.id << ee.runID << "velocity" << "RA" );
            }

            else if ( name == "get_experiment_info" )
            {  // us3_exp_procs.sql: experimentGUID, projectID, runID, labID,
               // instrumentID, operatorID, rotorID, rotorCalibrationID, type,
               // runTemp, label, comment, centrifugeProtocol, dateUpdated,
               // personID, runType, RIProfile
               if ( const FExp* ee = expById( a.value( 0 ) ) )
               {
                  QVariantList row;
                  row << ee->guid << ee->projID << ee->runID;
                  while ( row.size() < 17 )  row << QVariant();
                  row[ 8 ]  = "velocity";
                  row[ 10 ] = ee->runID + " label";
                  row[ 13 ] = kDate;
                  row[ 14 ] = ee->inv;
                  row[ 15 ] = "RA";
                  rows << row;
               }
            }

            else if ( name == "get_experiment_info_by_runID" )
            {
               for ( const FExp& ee : exps )
                  if ( ee.runID == a.value( 0 )  &&  QString::number( ee.inv ) == a.value( 1 ) )
                  {
                     QVariantList row;
                     row << ee.projID << ee.id << ee.guid;
                     while ( row.size() < 14 )  row << QVariant();
                     row[ 8 ]  = "velocity";
                     row[ 10 ] = ee.runID + " label";
                     row[ 13 ] = kDate;
                     rows << row;
                  }
            }

            else if ( name == "get_project_info" )
            {
               for ( const FExp& ee : exps )
                  if ( ee.projID == a.value( 0 ) )
                  {
                     QVariantList row;
                     while ( row.size() < 11 )  row << QVariant();
                     row[ 0 ]  = ee.projID;
                     row[ 1 ]  = ee.projGUID;
                     row[ 10 ] = ee.projDesc;
                     rows << row;
                     break;
                  }
            }

            else if ( name == "get_rawData_by_person" )
            {
               for ( const FRaw& rr : raws )
                  if ( mine( rr.expID, a.value( 0 ) ) )  rows << rawRow( rr, false );
            }

            else if ( name == "get_editedData_by_person" )
            {
               for ( const FEdit& ed : edits )
                  if ( ! rawById( ed.rawID )  ||  mine( rawById( ed.rawID )->expID, a.value( 0 ) ) )
                     rows << editRow( ed, false );
            }

            else if ( name == "get_model_info" )
            {
               for ( const FModel& mm : models )
                  if ( mm.id == a.value( 0 ) )
                     rows << ( QVariantList() << mm.guid << mm.desc.left( kDescWidth )
                                              << "<?xml version=\"1.0\"?><ModelData version=\"1.0\">"
                                                 "<model description=\"" + mm.desc + "\" modelGUID=\""
                                                 + mm.guid + "\"/></ModelData>" );
            }

            else if ( name == "get_noise_info" )
            {
               for ( const FNoise& nn : noises )
                  if ( nn.id == a.value( 0 ) )
                     // us3_noise_procs.sql: noiseGUID, editedDataID, modelID,
                     // modelGUID, noiseType, xml, ...
                     rows << ( QVariantList() << nn.guid << nn.editID << nn.modelID
                                              << nn.modelGUID << nn.type
                                              << "<?xml version=\"1.0\"?><NoiseData version=\"1.0\">"
                                                 "<noise type=\"ti\" description=\"" + nn.desc
                                                 + "\"/></NoiseData>" );
            }

            else if ( name == "get_modelID" )
            {
               for ( const FModel& mm : models )
                  if ( ! mm.guid.isEmpty()  &&  mm.guid == a.value( 0 ) )
                     rows << ( QVariantList() << mm.id );
            }

            else if ( name == "get_rawData_by_experiment" )
            {
               for ( const FRaw& rr : raws )
                  if ( rr.expID == a.value( 0 ) )  rows << rawRow( rr, true );
            }

            else if ( name == "get_editedData_by_experiment" )
            {
               for ( const FEdit& ed : edits )
                  if ( rawById( ed.rawID ) && rawById( ed.rawID )->expID == a.value( 0 ) )
                     rows << editRow( ed, true );
            }

            // Every model listing is of the investigator's models, and every
            // noise listing of the noise of those models, by noise.modelID.
            // A model whose edit is not in the store stands for one fitted to
            // an edit that is not the investigator's, which the real listing
            // returns with that edit's GUID.
            else if ( name == "get_model_desc" )
            {
               for ( const FModel& mm : models )
                  if ( QString::number( mm.inv ) == a.value( 0 ) )  rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc" )
            {
               for ( const FNoise& nn : noises )
                  if ( modelOf( nn.modelID, a.value( 0 ) ) )  rows << noiseRow( nn );
            }

            else if ( name == "get_model_desc_by_experiment" )
            {
               for ( const FModel& mm : models )
                  if ( expOfEdit( mm.editID ) == a.value( 1 )  &&
                       QString::number( mm.inv ) == a.value( 0 ) )  rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc_by_experiment" )
            {
               for ( const FNoise& nn : noises )
                  if ( expOfEdit( nn.editID ) == a.value( 1 )  &&
                       modelOf( nn.modelID, a.value( 0 ) ) )  rows << noiseRow( nn );
            }

            else if ( name == "get_model_desc_by_editID" )
            {
               for ( const FModel& mm : models )
                  if ( mm.editID == a.value( 1 )  &&
                       QString::number( mm.inv ) == a.value( 0 ) )  rows << modelRow( mm );
            }

            else if ( name == "get_noise_desc_by_editID" )
            {
               for ( const FNoise& nn : noises )
                  if ( nn.editID == a.value( 1 )  &&
                       modelOf( nn.modelID, a.value( 0 ) ) )  rows << noiseRow( nn );
            }

            else if ( name == "all_rawDataIDs" )
            {
               for ( const FRaw& rr : raws )
                  if ( mine( rr.expID, a.value( 0 ) ) )
                     rows << ( QVariantList() << rr.id << "label" << rr.file << rr.expID
                                              << rr.solID << kDate << cksum( rr.guid ) << "100"
                                              << kInv << rr.guid << rr.comment
                                              << ( expById( rr.expID ) ? expById( rr.expID )->guid : "" ) );
            }

            else if ( name == "all_editedDataIDs" )
               for ( const FEdit& ed : edits )
                  rows << ( QVariantList() << ed.id << "label" << ed.file << ed.rawID
                                           << ( rawById( ed.rawID ) ? rawById( ed.rawID )->expID : "" )
                                           << kDate << cksum( ed.guid ) << "200"
                                           << "velocity" << ed.guid );

            else if ( name == "get_rawDataIDs" )
            {
               for ( const FRaw& rr : raws )
                  if ( rr.expID == a.value( 0 ) )
                     rows << ( QVariantList() << rr.id << "label" << rr.file );
            }

            else if ( name == "get_rawData" )
            {
               if ( const FRaw* rr = rawById( a.value( 0 ) ) )
                  rows << ( QVariantList() << rr->guid << "label" << rr->file << rr->comment
                                           << rr->expID << rr->solID << "" << kDate
                                           << cksum( rr->guid ) << "100" );
            }

            else if ( name == "get_editedDataIDs" )
            {
               for ( const FEdit& ed : edits )
                  if ( ed.rawID == a.value( 0 ) )
                     rows << ( QVariantList() << ed.id << "label" << ed.file );
            }

            else if ( name == "get_editedData" )
            {
               for ( const FEdit& ed : edits )
                  if ( ed.id == a.value( 0 ) )
                     rows << ( QVariantList() << ed.rawID << ed.guid << "label" << ed.file
                                              << "comment" << kDate << cksum( ed.guid ) << "200" );
            }

            return rows;
         }
   };

   // Puts the selected investigator back however a test ends
   class InvestigatorGuard
   {
      public:
         explicit InvestigatorGuard( int id ) : saved( US_Settings::us_inv_ID() )
         {
            US_Settings::set_us_inv_ID( id );
         }
         ~InvestigatorGuard() { US_Settings::set_us_inv_ID( saved ); }

      private:
         int saved;
   };

   // A chain flattened to "kind:guid<parent" lines, for comparing two reads
   QStringList shape( const US_DataCatalog& catalog )
   {
      QStringList lines;

      for ( const US_DataCatalog::Run& run : catalog.runs() )
      {
         lines << "run:" + run.runID;

         for ( const US_DataCatalog::Raw& raw : run.raws )
         {
            lines << "  raw:" + raw.guid + " " + raw.filename + " " + raw.runID;

            for ( const US_DataCatalog::Edit& edit : raw.edits )
            {
               lines << "    edit:" + edit.guid + "<" + edit.rawGUID + " " + edit.runID;

               for ( const US_DataCatalog::Model& model : edit.models )
               {
                  lines << "      model:" + model.guid + "<" + model.editGUID;

                  for ( const US_DataCatalog::Noise& noise : model.noises )
                     lines << "        noise:" + noise.guid + "<" + noise.modelGUID
                              + "<" + noise.editGUID;
               }
            }
         }
      }

      return lines;
   }

   QStringList guids( const QList< US_DataCatalog::Model >& list )
   {
      QStringList out;
      for ( const auto& item : list )  out << item.guid;
      out.sort();
      return out;
   }

   QStringList guids( const QList< US_DataCatalog::Noise >& list )
   {
      QStringList out;
      for ( const auto& item : list )  out << item.guid;
      out.sort();
      return out;
   }

   QStringList guids( const QList< US_DataCatalog::Edit >& list )
   {
      QStringList out;
      for ( const auto& item : list )  out << item.guid;
      out.sort();
      return out;
   }
}

class TestUSDataCatalogMock : public QtTestBase
{
   protected:
      NiceMock< US_DB2_Mock >             db;
      FakeServer                          server;
      std::unique_ptr< InvestigatorGuard > inv;

      void SetUp() override
      {
         QtTestBase::SetUp();
         inv.reset( new InvestigatorGuard( kInv ) );
         server.wire( db );

         // Two experiments of one project.  RunA has two triples; the
         // first has two edits, each with a model, and one model has a
         // noise record.  RunB has one triple with one edit.
         server.exps << FExp{ "11", "exp-a", "RunA", "7", "proj-1", "Project one" }
                     << FExp{ "12", "exp-b", "RunB", "7", "proj-1", "Project one" };

         server.raws << FRaw{ "101", "raw-1", "RunA.RA.1.A.280.auc", "11", "comment A", "41" }
                     << FRaw{ "102", "raw-2", "RunA.RA.1.A.281.auc", "11", "comment A", "41" }
                     << FRaw{ "103", "raw-3", "RunB.RA.2.B.260.auc", "12", "comment B", "42" };

         server.edits << FEdit{ "201", "edit-1", "101", "RunA.2401011200.RA.1.A.280.xml" }
                      << FEdit{ "202", "edit-2", "101", "RunA.2401011300.RA.1.A.280.xml" }
                      << FEdit{ "203", "edit-3", "103", "RunB.2401011400.RA.2.B.260.xml" };

         server.models << FModel{ "301", "model-1", "RunA.1A280.e2401011200_a2401021300_2DSA_i01.model",
                                  "edit-1", "201" }
                       << FModel{ "302", "model-2", "RunA.1A280.e2401011300_a2401021400_GA_i01.model",
                                  "edit-2", "202" };

         server.noises << FNoise{ "401", "noise-1", "201", "301", "model-1", "ti_noise",
                                  "RunA.1A280.e2401011200_a2401021300_2DSA_i01.ti_noise" };
      }

      void TearDown() override
      {
         inv.reset();
         QtTestBase::TearDown();
      }

      // Records that attach to nothing, as a real store has them
      void addOrphans( void )
      {
         // a triple of an experiment that is not listed, with an edit
         server.raws  << FRaw { "109", "raw-9", "Gone.RA.1.A.280.auc", "99", "", "" };
         server.edits << FEdit{ "209", "edit-9", "109", "Gone.2401010000.RA.1.A.280.xml" };
         // an edit of a triple that is not there
         server.edits << FEdit{ "208", "edit-8", "999", "Lost.2401010000.RA.1.A.280.xml" };
         // a model of an edit that is not there, with a noise record
         server.models << FModel{ "308", "model-8", "Lost.model", "edit-x", "998" };
         server.noises << FNoise{ "408", "noise-8", "998", "308", "model-8", "ri_noise", "Lost.ri" };
         // a model with no edit at all, and one with no GUID
         server.models << FModel{ "307", "model-7", "NoEdit.model", "", "" };
         server.models << FModel{ "306", "", "NoGuid.model", "", "" };
         // noise of the model with no edit that names it only by modelID,
         // as a record whose modelGUID column is null does
         server.noises << FNoise{ "407", "noise-7", "", "307", "", "ti_noise", "Gone.ti" };
         // noise with no model at all: no server lists it, since every noise
         // listing joins modelPerson on noise.modelID
         server.noises << FNoise{ "406", "noise-6", "", "", "", "ti_noise", "NoModel.ti" };
      }

      void attachAndList( US_DataCatalog& catalog )
      {
         QString error;
         ASSERT_TRUE( catalog.attach( &db, error ) ) << error.toStdString();
         ASSERT_TRUE( catalog.loadRuns( error ) )    << error.toStdString();
         ASSERT_EQ  ( catalog.runCount(), 2 );
      }

      // The chain as one experiment at a time reads it, all procedures on
      QStringList referenceShape( void )
      {
         NiceMock< US_DB2_Mock > other;
         FakeServer              copy = server;
         copy.missing.clear();
         copy.denied .clear();
         copy.failOn .clear();
         copy.wire( other );

         US_DataCatalog catalog;
         QString        error;
         EXPECT_TRUE( catalog.attach( &other, error ) );
         EXPECT_TRUE( catalog.loadRuns( error ) );

         for ( int ii = 0; ii < catalog.runCount(); ii++ )
            EXPECT_TRUE( catalog.loadRunDetail( ii, error ) ) << error.toStdString();

         return shape( catalog );
      }
};

// ------------------------------------------------------------- connecting

TEST_F( TestUSDataCatalogMock, AConnectionThatIsNotConnectedIsRefused )
{
   NiceMock< US_DB2_Mock > down;
   ON_CALL( down, isConnected() ).WillByDefault( Return( false ) );

   US_DataCatalog catalog;
   QString        error;

   EXPECT_FALSE( catalog.attach( &down, error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_FALSE( catalog.isDb() );
   EXPECT_EQ   ( catalog.db(), nullptr );

   EXPECT_FALSE( catalog.attach( nullptr, error ) );
   EXPECT_FALSE( error.isEmpty() );
}

TEST_F( TestUSDataCatalogMock, TheAttachedConnectionIsTheOneUsed )
{
   US_DataCatalog catalog;
   QString        error;

   ASSERT_TRUE( catalog.attach( &db, error ) );
   EXPECT_EQ  ( catalog.db(), &db );
   EXPECT_TRUE( catalog.isDb() );
}

// --------------------------------------------------------------- fast path

TEST_F( TestUSDataCatalogMock, TheWholeStoreIsReadInFourQueries )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   server.calls.clear();
   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   EXPECT_EQ( server.calls, QStringList() << "get_rawData_by_person"
                                          << "get_editedData_by_person"
                                          << "get_model_desc" << "get_noise_desc" );
   EXPECT_EQ( shape( catalog ), referenceShape() );
   EXPECT_EQ( catalog.tiers(), int( US_DataCatalog::AllTiers ) );

   // these procedures do not hash the blobs, so nothing is verified
   EXPECT_FALSE( catalog.isRunVerified( 0 ) );
   EXPECT_TRUE ( catalog.run( 0 ).raws[ 0 ].checksum.isEmpty() );
}

// Every column of every procedure lands where the header says it does
TEST_F( TestUSDataCatalogMock, ColumnsAreMappedToTheRightFields )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   const US_DataCatalog::Run& listed = catalog.run( 0 );
   EXPECT_EQ( listed.id,          QString( "11" ) );
   EXPECT_EQ( listed.guid,        QString( "exp-a" ) );
   EXPECT_EQ( listed.runID,       QString( "RunA" ) );
   EXPECT_EQ( listed.expType,     QString( "velocity" ) );
   EXPECT_EQ( listed.runType,     QString( "RA" ) );
   EXPECT_EQ( listed.label,       QString( "RunA label" ) );
   EXPECT_EQ( listed.projectID,   QString( "7" ) );
   EXPECT_EQ( listed.projectGUID, QString( "proj-1" ) );
   EXPECT_EQ( listed.projectDesc, QString( "Project one" ) );
   EXPECT_EQ( listed.date,        kUtc );
   EXPECT_EQ( listed.rawCount,    2 );
   EXPECT_EQ( listed.editCount,   2 );
   EXPECT_EQ( listed.modelCount,  2 );
   EXPECT_EQ( listed.noiseCount,  1 );
   EXPECT_TRUE( listed.usable );

   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();

   const US_DataCatalog::Run& run = catalog.run( 0 );
   ASSERT_EQ( run.raws.size(), 2 );

   const US_DataCatalog::Raw& raw = run.raws[ 0 ];
   EXPECT_EQ( raw.id,           QString( "101" ) );
   EXPECT_EQ( raw.guid,         QString( "raw-1" ) );
   EXPECT_EQ( raw.filename,     QString( "RunA.RA.1.A.280.auc" ) );
   EXPECT_EQ( raw.triple,       QString( "1.A.280" ) );
   EXPECT_EQ( raw.dataType,     QString( "RA" ) );
   EXPECT_EQ( raw.runID,        QString( "RunA" ) );
   EXPECT_EQ( raw.experimentID, QString( "11" ) );
   EXPECT_EQ( raw.description,  QString( "comment A" ) );
   EXPECT_EQ( raw.solutionID,   QString( "41" ) );
   EXPECT_EQ( raw.lastUpdated,  kUtc );
   EXPECT_EQ( raw.checksum,     cksum( "raw-1" ) );
   EXPECT_EQ( raw.size,         QString( "100" ) );

   ASSERT_EQ( raw.edits.size(), 2 );
   const US_DataCatalog::Edit& edit = raw.edits[ 0 ];
   EXPECT_EQ( edit.id,          QString( "201" ) );
   EXPECT_EQ( edit.guid,        QString( "edit-1" ) );
   EXPECT_EQ( edit.rawGUID,     QString( "raw-1" ) );
   EXPECT_EQ( edit.filename,    QString( "RunA.2401011200.RA.1.A.280.xml" ) );
   EXPECT_EQ( edit.editID,      QString( "2401011200" ) );
   EXPECT_EQ( edit.triple,      QString( "1.A.280" ) );
   EXPECT_EQ( edit.runID,       QString( "RunA" ) );
   EXPECT_EQ( edit.lastUpdated, kUtc );
   EXPECT_EQ( edit.checksum,    cksum( "edit-1" ) );
   EXPECT_EQ( edit.size,        QString( "200" ) );

   ASSERT_EQ( edit.models.size(), 1 );
   const US_DataCatalog::Model& model = edit.models[ 0 ];
   EXPECT_EQ( model.id,          QString( "301" ) );
   EXPECT_EQ( model.guid,        QString( "model-1" ) );
   EXPECT_EQ( model.editGUID,    QString( "edit-1" ) );
   EXPECT_EQ( model.editID,      QString( "201" ) );
   EXPECT_EQ( model.subType,     QString( "2DSA" ) );
   EXPECT_EQ( model.lastUpdated, kUtc );
   EXPECT_EQ( model.checksum,    cksum( "model-1" ) );
   EXPECT_EQ( model.size,        QString( "300" ) );

   ASSERT_EQ( model.noises.size(), 1 );
   const US_DataCatalog::Noise& noise = model.noises[ 0 ];
   EXPECT_EQ( noise.id,          QString( "401" ) );
   EXPECT_EQ( noise.guid,        QString( "noise-1" ) );
   EXPECT_EQ( noise.noiseType,   QString( "ti" ) );
   EXPECT_EQ( noise.modelGUID,   QString( "model-1" ) );
   EXPECT_EQ( noise.editGUID,    QString( "edit-1" ) );
   EXPECT_EQ( noise.lastUpdated, kUtc );
   EXPECT_EQ( noise.checksum,    cksum( "noise-1" ) );
   EXPECT_EQ( noise.size,        QString( "400" ) );
   EXPECT_EQ( noise.description, QString( "RunA.1A280.e2401011200_a2401021300_2DSA_i01.ti_noise" ) );
}

// ------------------------------------------------------- legacy whole store

// A server without the whole-store catalog procedures is read with the
// whole-store procedures it has always had -- four queries, not one per
// record -- and gives the same chain.
TEST_F( TestUSDataCatalogMock, AnOldServerIsStillReadInFourQueries )
{
   server.missing << "get_rawData_by_person" << "get_editedData_by_person";

   US_DataCatalog catalog;
   attachAndList( catalog );

   server.calls.clear();
   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   EXPECT_EQ( server.calls, QStringList() << "get_rawData_by_person"
                                          << "all_rawDataIDs" << "all_editedDataIDs"
                                          << "get_model_desc" << "get_noise_desc" );
   EXPECT_EQ( shape( catalog ), referenceShape() );
   EXPECT_EQ( catalog.tiers() & US_DataCatalog::StoreTier, 0 );

   // the old procedures hash every blob, so the checksums came for free
   EXPECT_TRUE( catalog.isRunVerified( 0 ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].checksum, cksum( "raw-1" ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].edits[ 0 ].checksum, cksum( "edit-1" ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].description, QString( "comment A" ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].lastUpdated, kUtc );

   // and once known to be missing, they are not asked for again
   server.calls.clear();
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) );
   EXPECT_EQ( server.count( "get_rawData_by_person" ), 0 );
}

// A partly upgraded server: the first whole-store procedure is there and
// the second is not.  The read falls back rather than failing.
TEST_F( TestUSDataCatalogMock, APartlyUpgradedServerFallsBackMidWay )
{
   server.missing << "get_editedData_by_person";

   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
   EXPECT_EQ  ( shape( catalog ), referenceShape() );
   EXPECT_EQ  ( server.count( "all_rawDataIDs" ), 1 );
}

TEST_F( TestUSDataCatalogMock, TheStoreTierCanBeTurnedOff )
{
   US_DataCatalog catalog;
   catalog.setTiers( US_DataCatalog::AllTiers & ~US_DataCatalog::StoreTier );
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
   EXPECT_EQ  ( server.count( "get_rawData_by_person" ), 0 );
   EXPECT_EQ  ( server.count( "all_rawDataIDs" ), 1 );
   EXPECT_EQ  ( shape( catalog ), referenceShape() );
}

// Turned off, an account without execute rights on the new procedures is
// the same as a server without them
TEST_F( TestUSDataCatalogMock, AProcedureTheAccountMayNotRunIsFallenBackFrom )
{
   server.denied << "get_experiment_summary" << "get_rawData_by_person";

   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
   EXPECT_EQ  ( shape( catalog ), referenceShape() );
   EXPECT_EQ  ( catalog.tiers() & ( US_DataCatalog::ListingTier |
                                    US_DataCatalog::StoreTier ), 0 );
}

// --------------------------------------------------------- orphans

TEST_F( TestUSDataCatalogMock, RecordsWithNoParentAreKeptAsOrphans )
{
   addOrphans();

   for ( int pass = 0; pass < 2; pass++ )
   {
      SCOPED_TRACE( pass == 0 ? "catalog procedures" : "old procedures" );

      if ( pass == 1 )
         server.missing << "get_rawData_by_person";

      US_DataCatalog catalog;
      attachAndList( catalog );

      QString error;
      ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

      const US_DataCatalog::Orphans& orphans = catalog.orphans();

      // the triple of the experiment that is not listed, with its edit
      ASSERT_EQ( orphans.raws.size(), 1 );
      EXPECT_EQ( orphans.raws[ 0 ].guid,         QString( "raw-9" ) );
      EXPECT_EQ( orphans.raws[ 0 ].experimentID, QString( "99" ) );
      ASSERT_EQ( orphans.raws[ 0 ].edits.size(), 1 );
      EXPECT_EQ( orphans.raws[ 0 ].edits[ 0 ].guid, QString( "edit-9" ) );

      EXPECT_EQ( guids( orphans.edits ),  QStringList() << "edit-8" );
      EXPECT_EQ( guids( orphans.models ), QStringList() << "" << "model-7" << "model-8" );

      // database noise always comes with its model, so none is an orphan
      EXPECT_TRUE( orphans.noises.isEmpty() ) << guids( orphans.noises ).join( " " ).toStdString();

      // the model of a missing edit keeps its noise, and noise that names
      // its model only by ID is found under it
      for ( const US_DataCatalog::Model& model : orphans.models )
         if ( model.guid == "model-8" )
         {
            ASSERT_EQ( model.noises.size(), 1 );
            EXPECT_EQ( model.noises[ 0 ].guid, QString( "noise-8" ) );
         }
         else if ( model.guid == "model-7" )
         {
            ASSERT_EQ( model.noises.size(), 1 );
            EXPECT_EQ( model.noises[ 0 ].guid,      QString( "noise-7" ) );
            EXPECT_EQ( model.noises[ 0 ].modelGUID, QString( "model-7" ) );
         }
         else
            EXPECT_TRUE( model.noises.isEmpty() )
               << "an empty model GUID matched noise with an empty model GUID";

      // and the chain itself is untouched by them
      EXPECT_EQ( shape( catalog ), referenceShape() );
   }
}

TEST_F( TestUSDataCatalogMock, AProjectFilterLeavesNoOrphans )
{
   addOrphans();

   US_DataCatalog catalog;
   QString        error;
   ASSERT_TRUE( catalog.attach( &db, error ) );
   ASSERT_TRUE( catalog.loadRuns( "proj-1", error ) );
   ASSERT_TRUE( catalog.loadAll( error ) );

   EXPECT_TRUE( catalog.orphans().isEmpty() );
}

// ------------------------------------------------- one experiment at a time

TEST_F( TestUSDataCatalogMock, OneExperimentIsReadWithTheCatalogProcedures )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   server.calls.clear();
   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();

   EXPECT_EQ( server.calls, QStringList() << "get_rawData_by_experiment"
                                          << "get_editedData_by_experiment"
                                          << "get_model_desc_by_experiment"
                                          << "get_noise_desc_by_experiment" );
   EXPECT_TRUE( catalog.isRunVerified( 0 ) );     // checksums are on
}

// The third procedure of four is missing: this experiment, and every one
// after it, is read the old way
TEST_F( TestUSDataCatalogMock, APartlyUpgradedServerFallsBackPerExperiment )
{
   server.missing << "get_model_desc_by_experiment";

   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
   EXPECT_EQ  ( catalog.tiers() & US_DataCatalog::ExperimentTier, 0 );
   EXPECT_GT  ( server.count( "get_rawData" ), 0 );

   server.calls.clear();
   ASSERT_TRUE( catalog.loadRunDetail( 1, error ) ) << error.toStdString();
   EXPECT_EQ  ( server.count( "get_rawData_by_experiment" ), 0 );

   EXPECT_EQ( shape( catalog ), referenceShape() );
}

TEST_F( TestUSDataCatalogMock, TheExperimentTierCanBeTurnedOff )
{
   US_DataCatalog catalog;
   catalog.setTiers( US_DataCatalog::AllTiers & ~US_DataCatalog::ExperimentTier );
   attachAndList( catalog );

   QString error;
   for ( int ii = 0; ii < catalog.runCount(); ii++ )
      ASSERT_TRUE( catalog.loadRunDetail( ii, error ) ) << error.toStdString();

   EXPECT_EQ( server.count( "get_rawData_by_experiment" ), 0 );
   EXPECT_EQ( shape( catalog ), referenceShape() );
}

// A procedure that is there but fails is an error, not a reason to fall back
TEST_F( TestUSDataCatalogMock, AFailingProcedureIsNotMistakenForAMissingOne )
{
   server.failOn[ "get_editedData_by_experiment" ] = 0;

   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   EXPECT_FALSE( catalog.loadRunDetail( 0, error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_FALSE( catalog.isRunLoaded( 0 ) );
   EXPECT_TRUE ( catalog.run( 0 ).raws.isEmpty() );
   EXPECT_NE   ( catalog.tiers() & US_DataCatalog::ExperimentTier, 0 );
   EXPECT_EQ   ( server.count( "get_rawData" ), 0 );
}

// ------------------------------------------------------- the old listing

TEST_F( TestUSDataCatalogMock, AnOldServerIsListedWithTheOldProcedures )
{
   server.missing << "get_experiment_summary";

   US_DataCatalog catalog;
   attachAndList( catalog );

   const US_DataCatalog::Run& run = catalog.run( 0 );
   EXPECT_EQ( run.runID,       QString( "RunA" ) );
   EXPECT_EQ( run.guid,        QString( "exp-a" ) );
   EXPECT_EQ( run.projectGUID, QString( "proj-1" ) );
   EXPECT_EQ( run.projectDesc, QString( "Project one" ) );
   EXPECT_EQ( run.date,        kUtc );
   EXPECT_EQ( run.rawCount,    -1 );
   EXPECT_EQ( server.count( "get_project_info" ), 1 );   // asked once per project
}

// A query that fails part way through the old listing fails the listing,
// and the listing the catalog had is kept
TEST_F( TestUSDataCatalogMock, AFailureInTheOldListingKeepsThePreviousOne )
{
   server.missing << "get_experiment_summary";

   US_DataCatalog catalog;
   attachAndList( catalog );

   server.failOn[ "get_experiment_info" ] = server.knt.value( "get_experiment_info" ) + 2;

   QString error;
   EXPECT_FALSE( catalog.loadRuns( error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_EQ   ( catalog.runCount(), 2 );
}

// The same for a query in the middle of the record-at-a-time read: the
// experiment is not marked read with half of its records
TEST_F( TestUSDataCatalogMock, AFailureInTheOldDetailReadLeavesTheExperimentUnread )
{
   US_DataCatalog catalog;
   catalog.setTiers( 0 );
   attachAndList( catalog );

   const QStringList names = QStringList() << "get_rawData" << "get_editedDataIDs"
                                           << "get_editedData"
                                           << "get_model_desc_by_editID"
                                           << "get_noise_desc_by_editID";

   for ( const QString& name : names )
   {
      SCOPED_TRACE( name.toStdString() );

      server.failOn.clear();
      server.failOn[ name ] = server.knt.value( name ) + 2;

      QString error;
      EXPECT_FALSE( catalog.loadRunDetail( 0, error ) );
      EXPECT_FALSE( error.isEmpty() );
      EXPECT_FALSE( catalog.isRunLoaded( 0 ) );
      EXPECT_TRUE ( catalog.run( 0 ).raws.isEmpty() );
   }

   server.failOn.clear();
   QString error;
   EXPECT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
}

// ---------------------------------------------------- loadAll is atomic

TEST_F( TestUSDataCatalogMock, AFailedWholeStoreReadChangesNothing )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) );
   ASSERT_TRUE( catalog.isRunVerified( 0 ) );
   QStringList before = shape( catalog );

   server.failOn[ "get_noise_desc" ] = 0;

   EXPECT_FALSE( catalog.loadAll( error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_EQ   ( shape( catalog ), before );
   EXPECT_TRUE ( catalog.isRunLoaded  ( 0 ) );
   EXPECT_TRUE ( catalog.isRunVerified( 0 ) );
   EXPECT_FALSE( catalog.isRunLoaded  ( 1 ) );
   EXPECT_EQ   ( catalog.run( 0 ).raws[ 0 ].checksum, cksum( "raw-1" ) );

   // and once it succeeds, the experiment already read is kept as it was:
   // verified, with the checksums the fast listing does not have
   server.failOn.clear();
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.isRunVerified( 0 ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].checksum, cksum( "raw-1" ) );
   EXPECT_TRUE( catalog.isRunLoaded( 1 ) );
   EXPECT_FALSE( catalog.isRunVerified( 1 ) );
}

// ---------------------------------------------------------- checksums

TEST_F( TestUSDataCatalogMock, WithChecksumsOffADetailReadReportsNone )
{
   US_DataCatalog catalog;
   catalog.setChecksums( false );
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) );

   const US_DataCatalog::Run& run = catalog.run( 0 );
   EXPECT_FALSE( catalog.isRunVerified( 0 ) );
   EXPECT_TRUE ( run.raws[ 0 ].checksum.isEmpty() );
   EXPECT_TRUE ( run.raws[ 0 ].edits[ 0 ].checksum.isEmpty() );
   // models and noise are always hashed by the server as it lists them
   EXPECT_FALSE( run.raws[ 0 ].edits[ 0 ].models[ 0 ].checksum.isEmpty() );

   // the old whole-store read hashes too, and also reports nothing
   US_DataCatalog old;
   old.setChecksums( false );
   old.setTiers( 0 );
   attachAndList( old );
   ASSERT_TRUE ( old.loadAll( error ) );
   EXPECT_FALSE( old.isRunVerified( 0 ) );
   EXPECT_TRUE ( old.run( 0 ).raws[ 0 ].checksum.isEmpty() );
}

// ------------------------------------------------------- verification

TEST_F( TestUSDataCatalogMock, VerifyReadsTheChecksumsTheListingLeftOut )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE ( catalog.loadAll( error ) );
   ASSERT_FALSE( catalog.isRunVerified( 0 ) );

   server.calls.clear();
   ASSERT_TRUE( catalog.verifyRun( 0, error ) ) << error.toStdString();
   EXPECT_EQ  ( server.calls, QStringList() << "get_rawData_by_experiment"
                                            << "get_editedData_by_experiment" );
   EXPECT_TRUE( catalog.isRunVerified( 0 ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].checksum, cksum( "raw-1" ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].edits[ 1 ].checksum, cksum( "edit-2" ) );
}

// A server with the whole-store procedures but not the per-experiment
// ones verifies with the per-record procedures, which report checksums too
TEST_F( TestUSDataCatalogMock, VerifyWorksOnAServerWithoutThePerExperimentProcedures )
{
   server.missing << "get_rawData_by_experiment" << "get_editedData_by_experiment";

   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
   ASSERT_TRUE( catalog.verifyRun( 0, error ) ) << error.toStdString();

   EXPECT_TRUE( catalog.isRunVerified( 0 ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].checksum, cksum( "raw-1" ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 1 ].checksum, cksum( "raw-2" ) );
   EXPECT_EQ  ( catalog.run( 0 ).raws[ 0 ].edits[ 0 ].checksum, cksum( "edit-1" ) );
   EXPECT_EQ  ( catalog.tiers() & US_DataCatalog::ExperimentTier, 0 );

   // the next one goes straight to the per-record procedures
   server.calls.clear();
   ASSERT_TRUE( catalog.verifyRun( 1, error ) );
   EXPECT_EQ  ( server.count( "get_rawData_by_experiment" ), 0 );
   EXPECT_EQ  ( catalog.run( 1 ).raws[ 0 ].checksum, cksum( "raw-3" ) );
}

TEST_F( TestUSDataCatalogMock, AFailedVerifyLeavesTheExperimentUnverified )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) );

   server.failOn[ "get_editedData_by_experiment" ] = 0;
   EXPECT_FALSE( catalog.verifyRun( 0, error ) );
   EXPECT_FALSE( catalog.isRunVerified( 0 ) );
   EXPECT_TRUE ( catalog.run( 0 ).raws[ 0 ].checksum.isEmpty() )
      << "half of a verify was published";
}

// ------------------------------------------------------------ noise

TEST_F( TestUSDataCatalogMock, TheNoiseOfAModelIsCompleteAndReadOnce )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) );

   server.calls.clear();
   QList< US_DataCatalog::Noise > noises = catalog.noisesOfModel( "model-1", error );
   ASSERT_TRUE( error.isEmpty() ) << error.toStdString();
   ASSERT_EQ  ( noises.size(), 1 );

   const US_DataCatalog::Noise& noise = noises[ 0 ];
   EXPECT_EQ( noise.guid,        QString( "noise-1" ) );
   EXPECT_EQ( noise.editGUID,    QString( "edit-1" ) );
   EXPECT_EQ( noise.lastUpdated, kUtc );
   EXPECT_EQ( noise.checksum,    cksum( "noise-1" ) );
   EXPECT_EQ( noise.size,        QString( "400" ) );

   EXPECT_TRUE( catalog.noisesOfModel( "model-2", error ).isEmpty() );
   EXPECT_EQ  ( server.count( "get_noise_desc" ), 1 );

   // the noise of an edit carries the same
   QList< US_DataCatalog::Noise > ofEdit =
      catalog.noisesOfEdit( "edit-1", "201", error );
   ASSERT_EQ( ofEdit.size(), 1 );
   EXPECT_EQ( ofEdit[ 0 ].editGUID,    QString( "edit-1" ) );
   EXPECT_EQ( ofEdit[ 0 ].lastUpdated, kUtc );
   EXPECT_EQ( ofEdit[ 0 ].checksum,    noise.checksum );
}

// --------------------------------------------------------- the investigator

TEST_F( TestUSDataCatalogMock, ANewInvestigatorDropsTheOldListing )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QList< int > all;
   all << 0 << 1;
   catalog.queueVerify( all );

   InvestigatorGuard other( kInv + 1 );

   EXPECT_TRUE( catalog.refreshInvestigator() );
   EXPECT_EQ  ( catalog.runCount(), 0 );
   EXPECT_EQ  ( catalog.pendingCount(), 0 );
   EXPECT_EQ  ( catalog.verifyPendingCount(), 0 );
}

TEST_F( TestUSDataCatalogMock, ClearEmptiesBothQueues )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QList< int > all;
   all << 0 << 1;
   catalog.queueVerify( all );
   ASSERT_EQ( catalog.verifyPendingCount(), 2 );

   catalog.clear();
   EXPECT_EQ( catalog.verifyPendingCount(), 0 );
   EXPECT_EQ( catalog.pendingCount(), 0 );

   int     index = -1;
   QString error;
   EXPECT_FALSE( catalog.verifyNextPending( index, error ) );
   EXPECT_TRUE ( error.isEmpty() );
}

TEST_F( TestUSDataCatalogMock, DottedRunIdsAreListedButNotUsable )
{
   server.exps[ 1 ].runID = "Run.B";

   US_DataCatalog catalog;
   attachAndList( catalog );

   EXPECT_TRUE ( catalog.run( 0 ).usable );
   EXPECT_FALSE( catalog.run( 1 ).usable );
   EXPECT_FALSE( catalog.run( 1 ).problem.isEmpty() );
}

TEST( TestUSDataCatalogDates, DatesAreGivenInOneForm )
{
   EXPECT_EQ( US_DataCatalog::utcText( QVariant( QString( "2024-03-04 05:06:07" ) ) ),
              QString( "2024-03-04 05:06:07 UTC" ) );
   EXPECT_EQ( US_DataCatalog::utcText( QVariant( QString( "2024-03-04T05:06:07" ) ) ),
              QString( "2024-03-04 05:06:07 UTC" ) );
   EXPECT_EQ( US_DataCatalog::utcText(
                 QVariant( QDateTime( QDate( 2024, 3, 4 ), QTime( 5, 6, 7 ), Qt::UTC ) ) ),
              QString( "2024-03-04 05:06:07 UTC" ) );
   EXPECT_TRUE( US_DataCatalog::utcText( QVariant() ).isEmpty() );

   // a value with no time spec is the server's UTC as it stands
   EXPECT_EQ( US_DataCatalog::utcText(
                 QVariant( QDateTime( QDate( 2024, 3, 4 ), QTime( 5, 6, 7 ) ) ) ),
              QString( "2024-03-04 05:06:07 UTC" ) );
   EXPECT_EQ( US_DataCatalog::utcText( QVariant( QString( "2024-03-04 05:06:07 UTC" ) ) ),
              QString( "2024-03-04 05:06:07 UTC" ) );
}

// A UTC time the server gives is not read as a local time on the way: in a
// time zone that springs forward, 02:30 on that day does not exist locally
// and would come out shifted by an hour
TEST( TestUSDataCatalogDates, ATimeInTheLocalSpringForwardGapIsKept )
{
   const QByteArray saved   = qgetenv( "TZ" );
   const bool       hadTZ   = qEnvironmentVariableIsSet( "TZ" );
   qputenv( "TZ", "America/New_York" );
   tzset();

   // the zone is in effect: noon in January is 17:00 UTC there
   const int     noonUtc = QDateTime( QDate( 2024, 1, 1 ), QTime( 12, 0 ) ).toUTC().time().hour();
   const QString gap   = US_DataCatalog::utcText( QVariant( QString( "2024-03-10 02:30:00" ) ) );
   const QString gapT  = US_DataCatalog::utcText( QVariant( QString( "2024-03-10T02:30:00" ) ) );
   const QString fall  = US_DataCatalog::utcText( QVariant( QString( "2024-11-03 01:30:00" ) ) );

   if ( hadTZ )  qputenv( "TZ", saved );
   else          qunsetenv( "TZ" );
   tzset();

   EXPECT_EQ( noonUtc, 17 ) << "the time zone could not be set for this test";
   EXPECT_EQ( gap,   QString( "2024-03-10 02:30:00 UTC" ) );
   EXPECT_EQ( gapT,  QString( "2024-03-10 02:30:00 UTC" ) );
   EXPECT_EQ( fall,  QString( "2024-11-03 01:30:00 UTC" ) );
}

// ------------------------------------------------- the second review

// Two experiments of one investigator may share a run ID.  The old listing
// keeps each one's own ID and GUID -- from get_experiment_desc and
// get_experiment_info by ID, not by run ID -- so the records of both
// attach, and nothing of another investigator's is listed.
TEST_F( TestUSDataCatalogMock, AnOldServerListsExperimentsThatShareARunId )
{
   server.missing << "get_experiment_summary";

   FExp twin{ "13", "exp-c", "RunA", "7", "proj-1", "Project one" };
   FExp theirs{ "14", "exp-d", "RunA", "8", "proj-2", "Their project" };
   theirs.inv = kInv + 1;
   server.exps  << twin << theirs;
   server.raws  << FRaw { "104", "raw-4", "RunA.RA.3.A.280.auc", "13", "", "" }
                << FRaw { "105", "raw-5", "RunA.RA.4.A.280.auc", "14", "", "" };
   server.edits << FEdit{ "204", "edit-4", "104", "RunA.2401011500.RA.3.A.280.xml" };

   US_DataCatalog catalog;
   QString        error;
   ASSERT_TRUE( catalog.attach( &db, error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) ) << error.toStdString();
   ASSERT_EQ  ( catalog.runCount(), 3 );

   QStringList ids, exps;
   for ( const US_DataCatalog::Run& run : catalog.runs() )
   {
      ids  << run.id;
      exps << run.guid;
   }
   EXPECT_EQ( ids,  QStringList() << "11" << "12" << "13" );
   EXPECT_EQ( exps, QStringList() << "exp-a" << "exp-b" << "exp-c" );

   // label and date are read by ID too: this listing does not give them
   const US_DataCatalog::Run& twinRun = catalog.run( 2 );
   EXPECT_EQ( twinRun.runID,       QString( "RunA" ) );
   EXPECT_EQ( twinRun.label,       QString( "RunA label" ) );
   EXPECT_EQ( twinRun.date,        kUtc );
   EXPECT_EQ( twinRun.projectGUID, QString( "proj-1" ) );
   EXPECT_EQ( server.count( "get_experiment_info_by_runID" ), 0 );

   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.orphans().raws.isEmpty() )
      << "the twin's triple was taken for an orphan";
   ASSERT_EQ( catalog.run( 2 ).raws.size(), 1 );
   EXPECT_EQ( catalog.run( 2 ).raws[ 0 ].guid, QString( "raw-4" ) );
   EXPECT_EQ( catalog.run( 2 ).raws[ 0 ].edits.size(), 1 );
   EXPECT_EQ( catalog.run( 0 ).raws.size(), 2 );
}

// A project the server will not describe is not a reason to list nothing
TEST_F( TestUSDataCatalogMock, AnOldListingSurvivesAProjectItMayNotRead )
{
   server.missing << "get_experiment_summary";
   server.refused << "get_project_info";

   US_DataCatalog catalog;
   QSignalSpy     notes( &catalog, &US_DataCatalog::message );
   attachAndList( catalog );

   EXPECT_TRUE( catalog.run( 0 ).projectGUID.isEmpty() );
   EXPECT_EQ  ( catalog.run( 0 ).projectID, QString( "7" ) );
   EXPECT_EQ  ( catalog.run( 1 ).guid, QString( "exp-b" ) );
   EXPECT_GE  ( notes.count(), 1 );

   // but a connection that is gone is
   server.refused.clear();
   server.failOn[ "get_project_info" ] = 0;

   QString error;
   EXPECT_FALSE( catalog.loadRuns( error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_EQ   ( catalog.runCount(), 2 );
}

// With nothing listed the whole store is still read, and all of it is an
// orphan; loadAll used to return at once
TEST_F( TestUSDataCatalogMock, AStoreWithNoExperimentIsAllOrphans )
{
   server.exps.clear();

   US_DataCatalog catalog;
   QString        error;
   ASSERT_TRUE( catalog.attach( &db, error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_EQ  ( catalog.runCount(), 0 );

   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   const US_DataCatalog::Orphans& orphans = catalog.orphans();
   ASSERT_EQ( orphans.raws.size(), 3 );
   EXPECT_EQ( orphans.raws[ 0 ].guid, QString( "raw-1" ) );
   ASSERT_EQ( orphans.raws[ 0 ].edits.size(), 2 );
   EXPECT_EQ( orphans.raws[ 0 ].edits[ 0 ].models.size(), 1 );
   EXPECT_EQ( orphans.raws[ 0 ].edits[ 0 ].models[ 0 ].noises.size(), 1 );
}

// The server cuts descriptions at its 160-character column, and some
// noise was written with none; every read repairs them, on every tier
TEST_F( TestUSDataCatalogMock, DescriptionsAreRepairedForEveryReader )
{
   const QString longDesc = "RunA.1A280.e2401011200_a2401021300_2DSA_i01_"
                            + QString( 150, QChar( 'x' ) ) + ".model";
   const QString longNoise = QString( longDesc ).replace( ".model", ".ti_noise" );
   ASSERT_GT( longDesc.length(),  160 );
   ASSERT_GT( longNoise.length(), 160 );

   server.models[ 0 ].desc = longDesc;
   server.noises[ 0 ].desc = longNoise;
   server.noises << FNoise{ "402", "noise-2", "202", "302", "model-2", "ri_noise", "" };

   const QString model2   = server.models[ 1 ].desc;
   const QString noise2   = QString( model2 ).replace( ".model", ".ri_noise" );

   auto check = [ & ]( const US_DataCatalog& catalog )
   {
      const US_DataCatalog::Raw& raw = catalog.run( 0 ).raws[ 0 ];
      ASSERT_EQ( raw.edits.size(), 2 );
      ASSERT_EQ( raw.edits[ 0 ].models.size(), 1 );
      EXPECT_EQ( raw.edits[ 0 ].models[ 0 ].description, longDesc );
      EXPECT_EQ( raw.edits[ 0 ].models[ 0 ].subType, QString( "2DSA" ) );
      ASSERT_EQ( raw.edits[ 0 ].models[ 0 ].noises.size(), 1 );
      EXPECT_EQ( raw.edits[ 0 ].models[ 0 ].noises[ 0 ].description, longNoise );
      ASSERT_EQ( raw.edits[ 1 ].models[ 0 ].noises.size(), 1 );
      EXPECT_EQ( raw.edits[ 1 ].models[ 0 ].noises[ 0 ].description, noise2 );
   };

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
   {
      SCOPED_TRACE( tiers ? "catalog procedures" : "old procedures" );
      QString error;

      US_DataCatalog whole;
      whole.setTiers( tiers );
      attachAndList( whole );
      ASSERT_TRUE( whole.loadAll( error ) ) << error.toStdString();
      check( whole );

      US_DataCatalog one;
      one.setTiers( tiers );
      attachAndList( one );
      ASSERT_TRUE( one.loadRunDetail( 0, error ) ) << error.toStdString();
      check( one );

      QList< US_DataCatalog::Noise > ofModel = one.noisesOfModel( "model-2", error );
      ASSERT_EQ( ofModel.size(), 1 );
      EXPECT_EQ( ofModel[ 0 ].description, noise2 );

      QList< US_DataCatalog::Noise > ofEdit = one.noisesOfEdit( "edit-1", "201", error );
      ASSERT_EQ( ofEdit.size(), 1 );
      EXPECT_EQ( ofEdit[ 0 ].description, longNoise );
   }
}

TEST_F( TestUSDataCatalogMock, TheModelSubTypeIsTheAnalysisType )
{
   EXPECT_EQ( US_DataCatalog::modelSubType( "R.1A280.e1_a2_2DSA-MC_i01_mc0001.model" ),
              QString( "2DSA-MC" ) );
   EXPECT_EQ( US_DataCatalog::modelSubType( "R.x.1A280.e1_a2_PCSA_i01.model" ),
              QString( "PCSA" ) );
   EXPECT_TRUE( US_DataCatalog::modelSubType( "Lost.model" ).isEmpty() );
}

// Noise read with one experiment that hangs under none of its models is
// another experiment's: every noise listing joins modelPerson on
// noise.modelID, so its model exists.  It is hung under that model, found
// by GUID or -- when the record has none -- by modelID, whichever
// experiment is read first, and the counts agree with a whole-store read.
TEST_F( TestUSDataCatalogMock, StrayNoiseIsSortedAsTheWholeStoreReadSortsIt )
{
   server.models << FModel{ "303", "model-3", "RunB.2B260.e1_a2_2DSA_i01.model", "edit-3", "203" };
   server.noises << FNoise{ "413", "noise-s3", "201", "303", "model-3", "ti_noise", "s3" }
                 << FNoise{ "414", "noise-s4", "201", "303", "",        "ri_noise", "s4" };

   US_DataCatalog whole;
   attachAndList( whole );
   QString error;
   ASSERT_TRUE( whole.loadAll( error ) ) << error.toStdString();
   EXPECT_TRUE( whole.orphans().noises.isEmpty() );
   ASSERT_EQ  ( whole.run( 1 ).noiseCount, 2 );

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
      for ( bool forward : { true, false } )
      {
         SCOPED_TRACE( QString( "%1, %2 first" )
                       .arg( tiers ? "catalog procedures" : "old procedures" )
                       .arg( forward ? "RunA" : "RunB" ).toStdString() );

         US_DataCatalog one;
         one.setTiers( tiers );
         attachAndList( one );
         QSignalSpy changed( &one, &US_DataCatalog::runChanged );

         ASSERT_TRUE( one.loadRunDetail( forward ? 0 : 1, error ) ) << error.toStdString();
         ASSERT_TRUE( one.loadRunDetail( forward ? 1 : 0, error ) ) << error.toStdString();

         EXPECT_TRUE( one.orphans().noises.isEmpty() );
         EXPECT_EQ( one.run( 0 ).noiseCount, whole.run( 0 ).noiseCount );
         EXPECT_EQ( one.run( 1 ).noiseCount, whole.run( 1 ).noiseCount );
         EXPECT_EQ( shape( one ), shape( whole ) );

         // RunB read first has the count the listing gave it, and so
         // finds its noise recorded against RunA at once.  An old server's
         // listing gives no count: RunB is then changed by reading RunA,
         // and says so.
         if ( forward  ||  tiers != 0 )
            EXPECT_EQ( changed.count(), 0 );
         else
         {
            ASSERT_EQ( changed.count(), 1 );
            EXPECT_EQ( changed.at( 0 ).at( 0 ).toInt(), 1 );
         }
      }
}

// A model loaded with its experiment can have noise recorded against an
// edit of an experiment not read yet.  A whole-store read then finds it,
// and the loaded experiment gets it, rather than its being thrown away
// with the fresh copy of that experiment.
TEST_F( TestUSDataCatalogMock, AWholeStoreReadGivesLoadedRunsTheirLateNoise )
{
   server.noises << FNoise{ "415", "noise-u1", "203", "301", "model-1", "ti_noise", "u1" };

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
   {
      SCOPED_TRACE( tiers ? "catalog procedures" : "old procedures" );

      US_DataCatalog reference;
      reference.setTiers( tiers );
      attachAndList( reference );
      QString error;
      ASSERT_TRUE( reference.loadAll( error ) ) << error.toStdString();

      US_DataCatalog catalog;
      catalog.setTiers( tiers );
      attachAndList( catalog );
      ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
      // The listing counts it, so the catalog procedures' read finds it at
      // once; an old server's listing gives no count to go by
      ASSERT_EQ  ( catalog.run( 0 ).noiseCount, tiers ? 2 : 1 );

      QSignalSpy changed( &catalog, &US_DataCatalog::runChanged );
      ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

      EXPECT_EQ( catalog.run( 0 ).noiseCount, 2 );
      EXPECT_EQ( catalog.run( 0 ).noiseCount, reference.run( 0 ).noiseCount );
      EXPECT_EQ( shape( catalog ), shape( reference ) );
      if ( tiers )
         EXPECT_EQ( changed.count(), 0 );
      else
      {
         ASSERT_EQ( changed.count(), 1 );
         EXPECT_EQ( changed.at( 0 ).at( 0 ).toInt(), 0 );
      }

      // and a second read does not add it again
      changed.clear();
      ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();
      EXPECT_EQ  ( catalog.run( 0 ).noiseCount, 2 );
      EXPECT_EQ  ( changed.count(), 0 );
   }
}

// Without a listing every record would look like an orphan
TEST_F( TestUSDataCatalogMock, AWholeStoreReadNeedsAListing )
{
   US_DataCatalog catalog;
   QString        error;
   ASSERT_TRUE( catalog.attach( &db, error ) );

   server.calls.clear();
   EXPECT_FALSE( catalog.loadAll( error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_TRUE ( server.calls.isEmpty() ) << server.calls.join( " " ).toStdString();
   EXPECT_TRUE ( catalog.orphans().isEmpty() );

   // a listing that fails is no listing either
   server.failOn[ "get_experiment_summary" ] = 0;
   EXPECT_FALSE( catalog.loadRuns( error ) );
   EXPECT_FALSE( catalog.loadAll( error ) );
   EXPECT_TRUE ( catalog.orphans().isEmpty() );
   server.failOn.clear();

   ASSERT_TRUE( catalog.loadRuns( error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   // and clearing it, or a new investigator, drops it
   catalog.clear();
   EXPECT_FALSE( catalog.loadAll( error ) );

   ASSERT_TRUE( catalog.loadRuns( error ) );
   InvestigatorGuard other( kInv + 1 );
   ASSERT_TRUE( catalog.refreshInvestigator() );
   EXPECT_FALSE( catalog.loadAll( error ) );
}

// The description column is 160 wide.  One of 80 characters is whole and
// is not read again; one of 160 may be cut and is read again -- once,
// however many reads list it, and whatever that read gave.
TEST_F( TestUSDataCatalogMock, AFullWidthDescriptionIsReadAgainOnce )
{
   const QString eighty = "RunA.1A280.e2401011200_a2401021300_2DSA_i01_"
                          + QString( 30, QChar( 'y' ) ) + ".model";
   const QString exact  = "RunA.1A280.e2401011300_a2401021400_GA_i01_"
                          + QString( 112, QChar( 'z' ) ) + ".model";
   const QString noise  = "RunA.1A280.e2401011200_a2401021300_2DSA_i01_"
                          + QString( 107, QChar( 'n' ) ) + ".ti_noise";
   ASSERT_EQ( eighty.length(), 80 );
   ASSERT_EQ( exact .length(), 160 );
   ASSERT_EQ( noise .length(), 160 );

   server.models[ 0 ].desc = eighty;
   server.models[ 1 ].desc = exact;
   server.noises[ 0 ].desc = noise;

   US_DataCatalog catalog;
   attachAndList( catalog );
   QString error;

   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
   EXPECT_EQ( catalog.run( 0 ).raws[ 0 ].edits[ 0 ].models[ 0 ].description, eighty );
   EXPECT_EQ( catalog.run( 0 ).raws[ 0 ].edits[ 1 ].models[ 0 ].description, exact );
   EXPECT_EQ( server.count( "get_model_info" ), 1 ) << "only the full-width one is read";
   EXPECT_EQ( server.count( "get_noise_info" ), 1 );

   // the noise lookups do not read the models again, nor the noise
   for ( int pass = 0; pass < 2; pass++ )
   {
      QList< US_DataCatalog::Noise > ofEdit = catalog.noisesOfEdit( "edit-1", "201", error );
      ASSERT_EQ( ofEdit.size(), 1 );
      EXPECT_EQ( ofEdit[ 0 ].description, noise );
   }

   ASSERT_EQ( catalog.noisesOfModel( "model-1", error ).size(), 1 );
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   EXPECT_EQ( server.count( "get_model_info" ), 1 );
   EXPECT_EQ( server.count( "get_noise_info" ), 1 );

   // a new listing reads the store afresh
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_TRUE( catalog.loadAll ( error ) );
   EXPECT_EQ( server.count( "get_model_info" ), 2 );
}

// Database noise is attached by modelID, the column the server joins it to
// its model by, so a record whose modelGUID is null is still found under
// its model, by every reader
TEST_F( TestUSDataCatalogMock, NoiseWithNoModelGuidIsFoundByItsModelId )
{
   server.noises << FNoise{ "402", "noise-2", "202", "302", "", "ri_noise", "n2" };

   auto check = [ & ]( const US_DataCatalog& catalog )
   {
      const US_DataCatalog::Model& model = catalog.run( 0 ).raws[ 0 ].edits[ 1 ].models[ 0 ];
      ASSERT_EQ( model.guid, QString( "model-2" ) );
      ASSERT_EQ( model.noises.size(), 1 );
      EXPECT_EQ( model.noises[ 0 ].guid,      QString( "noise-2" ) );
      EXPECT_EQ( model.noises[ 0 ].modelGUID, QString( "model-2" ) );
      EXPECT_EQ( model.noises[ 0 ].modelID,   QString( "302" ) );
      EXPECT_EQ( model.noises[ 0 ].editGUID,  QString( "edit-2" ) );
      EXPECT_EQ( catalog.run( 0 ).noiseCount, 2 );
      EXPECT_TRUE( catalog.orphans().noises.isEmpty() );
   };

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
   {
      SCOPED_TRACE( tiers ? "catalog procedures" : "old procedures" );
      QString error;

      US_DataCatalog whole;
      whole.setTiers( tiers );
      attachAndList( whole );
      ASSERT_TRUE( whole.loadAll( error ) ) << error.toStdString();
      check( whole );
      EXPECT_EQ( whole.noisesOfModel( "model-2", error ).size(), 1 );

      US_DataCatalog one;
      one.setTiers( tiers );
      attachAndList( one );
      ASSERT_TRUE( one.loadRunDetail( 0, error ) ) << error.toStdString();
      check( one );
      EXPECT_EQ( one.noisesOfModel( "model-2", error ).size(), 1 );
   }
}

// A record whose parent GUID is empty is attached by its parent's ID, and
// the per-record and the per-experiment and whole-store procedures all
// attach it to the same parent
TEST_F( TestUSDataCatalogMock, AnEmptyParentGuidIsAttachedByIdOnEveryTier )
{
   // a triple with no GUID, so its edit names none either, and a model
   // whose edit GUID is empty
   server.raws   << FRaw  { "104", "", "RunA.RA.2.A.280.auc", "11", "", "" };
   server.edits  << FEdit { "204", "edit-4", "104", "RunA.2401011500.RA.2.A.280.xml" };
   server.models << FModel{ "305", "model-5", "RunA.2A280.e2401011500_a1_2DSA_i01.model", "", "202" };

   QStringList reference;

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
   {
      SCOPED_TRACE( tiers ? "catalog procedures" : "old procedures" );
      QString error;

      US_DataCatalog one;
      one.setTiers( tiers );
      attachAndList( one );
      ASSERT_TRUE( one.loadRunDetail( 0, error ) ) << error.toStdString();
      ASSERT_TRUE( one.loadRunDetail( 1, error ) ) << error.toStdString();

      US_DataCatalog whole;
      whole.setTiers( tiers );
      attachAndList( whole );
      ASSERT_TRUE( whole.loadAll( error ) ) << error.toStdString();

      EXPECT_EQ( shape( one ), shape( whole ) );
      EXPECT_TRUE( whole.orphans().edits .isEmpty() );
      EXPECT_TRUE( whole.orphans().models.isEmpty() );

      const US_DataCatalog::Run& run = whole.run( 0 );
      ASSERT_EQ( run.raws.size(), 3 );
      EXPECT_EQ( run.editCount,  3 );
      EXPECT_EQ( run.modelCount, 3 );

      bool found = false;
      for ( const US_DataCatalog::Raw& raw : run.raws )
         for ( const US_DataCatalog::Edit& edit : raw.edits )
            for ( const US_DataCatalog::Model& model : edit.models )
               if ( model.guid == "model-5" )
               {
                  found = true;
                  EXPECT_EQ( edit.guid,      QString( "edit-2" ) );
                  EXPECT_EQ( model.editGUID, QString( "edit-2" ) );
               }
      EXPECT_TRUE( found );

      if ( reference.isEmpty() )
         reference = shape( whole );
      else
         EXPECT_EQ( shape( whole ), reference ) << "the tiers disagree";
   }
}

// A run ID is found without case where the source does not tell case apart
TEST_F( TestUSDataCatalogMock, ARunIdIsFoundWithoutCaseOnTheDatabase )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   EXPECT_EQ( catalog.indexOfRun( "RunB" ), 1 );
   EXPECT_EQ( catalog.indexOfRun( "runb" ), 1 );
   EXPECT_EQ( catalog.indexOfRun( "RUNA" ), 0 );
   EXPECT_EQ( catalog.indexOfRun( "RunC" ), -1 );

   QString error;
   EXPECT_TRUE( catalog.loadRunDetail( QString( "runa" ), error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.isRunLoaded( 0 ) );
}

// An exact match wins over one without case
TEST_F( TestUSDataCatalogMock, AnExactRunIdMatchWins )
{
   server.exps << FExp{ "13", "exp-c", "runa", "7", "proj-1", "Project one" };

   US_DataCatalog catalog;
   QString        error;
   ASSERT_TRUE( catalog.attach( &db, error ) );
   ASSERT_TRUE( catalog.loadRuns( error ) );
   ASSERT_EQ  ( catalog.runCount(), 3 );

   int lower = catalog.indexOfRun( "runa" );
   int upper = catalog.indexOfRun( "RunA" );
   ASSERT_GE( lower, 0 );
   ASSERT_GE( upper, 0 );
   EXPECT_EQ( catalog.run( lower ).runID, QString( "runa" ) );
   EXPECT_EQ( catalog.run( upper ).runID, QString( "RunA" ) );
}

// Another investigator's models fitted to this investigator's edits are
// not listed, nor is their noise, as the server's modelPerson join has it
TEST_F( TestUSDataCatalogMock, AnotherInvestigatorsModelsAreNotListed )
{
   FModel theirs{ "320", "model-t", "RunA.1A280.e2401011200_a9_2DSA_i01.model", "edit-1", "201" };
   theirs.inv = kInv + 1;
   server.models << theirs;
   server.noises << FNoise{ "420", "noise-t", "201", "320", "model-t", "ti_noise", "t" };

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
   {
      SCOPED_TRACE( tiers ? "catalog procedures" : "old procedures" );
      QString error;

      US_DataCatalog catalog;
      catalog.setTiers( tiers );
      attachAndList( catalog );

      if ( tiers )
      {  // the listing counts what the detail read will find
         EXPECT_EQ( catalog.run( 0 ).modelCount, 2 );
         EXPECT_EQ( catalog.run( 0 ).noiseCount, 1 );
      }

      ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
      EXPECT_EQ( catalog.run( 0 ).modelCount, 2 );
      EXPECT_EQ( catalog.run( 0 ).noiseCount, 1 );
      EXPECT_FALSE( shape( catalog ).join( " " ).contains( "model-t" ) );
      EXPECT_TRUE ( catalog.orphans().isEmpty() );
   }
}

// A read that fails is tried once more, after the rest; a second failure
// drops it
TEST_F( TestUSDataCatalogMock, AFailedReadIsRetriedOnce )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   server.failOn[ "get_rawData_by_experiment" ] = server.knt.value( "get_rawData_by_experiment" ) + 1;

   int     index = -1;
   QString error;
   EXPECT_FALSE( catalog.loadNextPending( index, error ) );
   EXPECT_EQ   ( index, 0 );
   EXPECT_TRUE ( error.contains( "again" ) ) << error.toStdString();
   EXPECT_EQ   ( catalog.pendingCount(), 2 );

   EXPECT_TRUE( catalog.loadNextPending( index, error ) ) << error.toStdString();
   EXPECT_EQ  ( index, 1 );
   EXPECT_TRUE( catalog.loadNextPending( index, error ) ) << error.toStdString();
   EXPECT_EQ  ( index, 0 );
   EXPECT_EQ  ( catalog.pendingCount(), 0 );

   // failing every time, each experiment is tried twice and then dropped
   ASSERT_TRUE( catalog.loadRuns( error ) );
   server.failOn[ "get_rawData_by_experiment" ] = 0;

   int failures = 0;
   while ( catalog.pendingCount() > 0  &&  failures < 10 )
      if ( ! catalog.loadNextPending( index, error ) )  failures++;

   EXPECT_EQ( failures, 4 );
   EXPECT_FALSE( catalog.isRunLoaded( 0 ) );
}

TEST_F( TestUSDataCatalogMock, AFailedVerifyIsRetriedOnce )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) );
   catalog.queueVerify( QList< int >() << 0 << 1 );

   server.failOn[ "get_rawData_by_experiment" ] = server.knt.value( "get_rawData_by_experiment" ) + 1;

   int index = -1;
   EXPECT_FALSE( catalog.verifyNextPending( index, error ) );
   EXPECT_EQ   ( index, 0 );
   EXPECT_TRUE ( error.contains( "again" ) ) << error.toStdString();
   EXPECT_EQ   ( catalog.verifyPendingCount(), 2 );

   EXPECT_TRUE( catalog.verifyNextPending( index, error ) ) << error.toStdString();
   EXPECT_TRUE( catalog.verifyNextPending( index, error ) ) << error.toStdString();
   EXPECT_EQ  ( index, 0 );
   EXPECT_TRUE( catalog.isRunVerified( 0 ) );
   EXPECT_TRUE( catalog.isRunVerified( 1 ) );
}

// A model with an empty edit GUID is an orphan of the whole store, and
// its noise stays with it
TEST_F( TestUSDataCatalogMock, AModelWithNoEditGuidKeepsItsNoise )
{
   server.models << FModel{ "309", "model-9", "Lone.model", "", "" };
   server.noises << FNoise{ "409", "noise-9", "", "309", "model-9", "ti_noise", "" };

   US_DataCatalog catalog;
   attachAndList( catalog );
   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) );

   ASSERT_EQ( catalog.orphans().models.size(), 1 );
   ASSERT_EQ( catalog.orphans().models[ 0 ].noises.size(), 1 );
   // and the empty description was given the model's
   EXPECT_EQ( catalog.orphans().models[ 0 ].noises[ 0 ].description, QString( "Lone.ti_noise" ) );
   EXPECT_TRUE( catalog.orphans().noises.isEmpty() );
}

// ------------------------------------------------- where noise is counted

/* Global fits: noise recorded against an edit of RunA whose model is on the
   UNASSIGNED edit (editedDataID 1, which belongs to no experiment), and
   noise recorded against RunA whose model is in RunB.  The listing counts
   noise by its model's experiment, the catalog files it under its model,
   so no read order changes a count, and the UNASSIGNED model's noise is
   counted under no experiment but kept with its model in the orphans. */
class TestUSDataCatalogNoiseCounts : public TestUSDataCatalogMock
{
   protected:
      void SetUp() override
      {
         TestUSDataCatalogMock::SetUp();
         server.models << FModel{ "303", "model-3", "RunB.2B260.e1_a2_2DSA_i01.model", "edit-3", "203" }
                       << FModel{ "309", "model-global", "Global.e1_a2_2DSA-GL_i01.model",
                                  "00000000-0000-0000-0000-000000000000", "1" };
         server.noises << FNoise{ "421", "noise-g1", "201", "309", "model-global", "ti_noise", "g1" }
                       << FNoise{ "422", "noise-g2", "203", "309", "model-global", "ti_noise", "g2" }
                       << FNoise{ "423", "noise-b", "201", "303", "model-3", "ri_noise", "b" };
      }

      QList< int > listedNoise( US_DataCatalog& catalog )
      {
         QList< int > counts;
         for ( int ii = 0; ii < catalog.runCount(); ii++ )
            counts << catalog.run( ii ).noiseCount;
         return counts;
      }
};

TEST_F( TestUSDataCatalogNoiseCounts, LoadingChangesNoNoiseCount )
{
   for ( bool forward : { true, false } )
   {
      SCOPED_TRACE( forward ? "RunA first" : "RunB first" );
      US_DataCatalog catalog;
      attachAndList( catalog );
      const QList< int > listed = listedNoise( catalog );
      // noise-1 and noise-b's model are in RunA and RunB; the global
      // model's noise is in neither
      EXPECT_EQ( listed, QList< int >() << 1 << 1 );

      QString error;
      ASSERT_TRUE( catalog.loadRunDetail( forward ? 0 : 1, error ) ) << error.toStdString();
      ASSERT_TRUE( catalog.loadRunDetail( forward ? 1 : 0, error ) ) << error.toStdString();
      EXPECT_EQ( listedNoise( catalog ), listed );

      US_DataCatalog whole;
      attachAndList( whole );
      ASSERT_TRUE( whole.loadAll( error ) ) << error.toStdString();
      EXPECT_EQ( listedNoise( whole ), listed );
      EXPECT_EQ( shape( catalog ), shape( whole ) );
   }
}

// One experiment read alone has the count the listing gave it, even when
// noise of its models is recorded against another experiment's edits,
// which the per-experiment listing does not return with it
TEST_F( TestUSDataCatalogNoiseCounts, AnExperimentReadAloneKeepsItsCount )
{
   for ( int index : { 0, 1 } )
   {
      SCOPED_TRACE( index );
      US_DataCatalog catalog;
      attachAndList( catalog );
      const int listed = catalog.run( index ).noiseCount;
      QString   error;
      ASSERT_TRUE( catalog.loadRunDetail( index, error ) ) << error.toStdString();
      EXPECT_EQ( catalog.run( index ).noiseCount, listed );
   }

   // RunB's model-3 has noise-b, recorded against RunA
   US_DataCatalog catalog;
   attachAndList( catalog );
   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 1, error ) ) << error.toStdString();
   EXPECT_EQ( guids( catalog.run( 1 ).raws[ 0 ].edits[ 0 ].models[ 0 ].noises ), QStringList() << "noise-b" );
}

TEST_F( TestUSDataCatalogNoiseCounts, AGlobalFitModelIsAnOrphanWithItsNoise )
{
   US_DataCatalog catalog;
   attachAndList( catalog );
   QString error;
   ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

   ASSERT_EQ( guids( catalog.orphans().models ), QStringList() << "model-global" );
   const US_DataCatalog::Model& global = catalog.orphans().models[ 0 ];
   EXPECT_EQ( global.editID, QString( "1" ) );
   EXPECT_EQ( guids( global.noises ), QStringList() << "noise-g1" << "noise-g2" );
   EXPECT_TRUE( catalog.orphans().noises.isEmpty() );

   EXPECT_EQ( guids( catalog.noisesOfModel( "model-global", error ) ),
              QStringList() << "noise-g1" << "noise-g2" );
}

// The first catalog procedures counted noise by the edit it was recorded
// against; there a count changes on loading, as documented
TEST_F( TestUSDataCatalogNoiseCounts, TheFirstSummaryCountedByEdit )
{
   server.countNoiseByEdit = true;
   US_DataCatalog catalog;
   attachAndList( catalog );
   // RunA's edit holds noise-1, noise-g1 and noise-b; RunB's noise-g2
   EXPECT_EQ( listedNoise( catalog ), QList< int >() << 3 << 1 );

   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
   ASSERT_TRUE( catalog.loadRunDetail( 1, error ) ) << error.toStdString();
   EXPECT_EQ( listedNoise( catalog ), QList< int >() << 1 << 1 );
}

// ---------------------------------------------------------- fixes, round 4

// A record attached by ID, for want of a GUID, is verified by ID too; it
// used to keep no checksum while the experiment was called verified
TEST_F( TestUSDataCatalogMock, VerifyFindsARecordWithNoGuidByItsId )
{
   server.raws  << FRaw { "104", "", "RunA.RA.2.A.280.auc", "11", "", "" };
   server.edits << FEdit{ "204", "", "104", "RunA.2401011500.RA.2.A.280.xml" };

   US_DataCatalog catalog;
   catalog.setChecksums( false );
   attachAndList( catalog );
   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
   ASSERT_TRUE( catalog.verifyRun( 0, error ) ) << error.toStdString();

   bool found = false;
   for ( const US_DataCatalog::Raw& raw : catalog.run( 0 ).raws )
   {
      EXPECT_FALSE( raw.checksum.isEmpty() ) << raw.id.toStdString();
      for ( const US_DataCatalog::Edit& edit : raw.edits )
         EXPECT_FALSE( edit.checksum.isEmpty() ) << edit.id.toStdString();
      found = found  ||  raw.id == "104";
   }
   EXPECT_TRUE( found );
}

// A record the server no longer lists cannot be verified
TEST_F( TestUSDataCatalogMock, VerifyFailsForARecordThatIsGone )
{
   US_DataCatalog catalog;
   catalog.setChecksums( false );
   attachAndList( catalog );
   QString error;
   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();

   server.edits.removeAt( 1 );   // edit-2 is deleted meanwhile
   EXPECT_FALSE( catalog.verifyRun( 0, error ) );
   EXPECT_TRUE( error.contains( "RunA.2401011300" ) ) << error.toStdString();
   EXPECT_FALSE( catalog.isRunVerified( 0 ) );
}

// The noise listing is read once, but which model each record is of is
// worked out when it is asked: a model read after it counts too
TEST_F( TestUSDataCatalogMock, TheNoiseOfAModelReadLaterIsFound )
{
   server.noises << FNoise{ "402", "noise-2", "202", "302", "", "ri_noise", "" };

   US_DataCatalog catalog;
   attachAndList( catalog );
   QString error;
   EXPECT_EQ( catalog.noisesOfModel( "model-1", error ).size(), 1 );

   ASSERT_TRUE( catalog.loadRunDetail( 0, error ) ) << error.toStdString();
   QList< US_DataCatalog::Noise > noises = catalog.noisesOfModel( "model-2", error );
   ASSERT_EQ( noises.size(), 1 );
   EXPECT_EQ( noises[ 0 ].guid,        QString( "noise-2" ) );
   EXPECT_EQ( noises[ 0 ].modelGUID,   QString( "model-2" ) );
   EXPECT_EQ( noises[ 0 ].editGUID,    QString( "edit-2" ) );
   EXPECT_EQ( noises[ 0 ].description, QString( "RunA.1A280.e2401011300_a2401021400_GA_i01.ri_noise" ) );
   EXPECT_EQ( server.count( "get_noise_desc" ), 1 );
}

// Noise read with one experiment and hung under a model of another takes
// that model's description when it has none, as noise read with its model
// does
TEST_F( TestUSDataCatalogMock, ForeignNoiseIsDescribedAfterItsModel )
{
   server.models << FModel{ "303", "model-3", "RunB.2B260.e1_a2_2DSA_i01.model", "edit-3", "203" };
   server.noises << FNoise{ "413", "noise-s3", "201", "303", "model-3", "ti_noise", "" };

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
      for ( bool forward : { true, false } )
      {
         SCOPED_TRACE( QString( "%1, %2 first" )
                          .arg( tiers ? "catalog procedures" : "old procedures" )
                          .arg( forward ? "RunA" : "RunB" )
                          .toStdString() );
         US_DataCatalog catalog;
         catalog.setTiers( tiers );
         attachAndList( catalog );
         QString error;
         ASSERT_TRUE( catalog.loadRunDetail( forward ? 0 : 1, error ) ) << error.toStdString();
         ASSERT_TRUE( catalog.loadRunDetail( forward ? 1 : 0, error ) ) << error.toStdString();

         const US_DataCatalog::Model& model = catalog.run( 1 ).raws[ 0 ].edits[ 0 ].models[ 0 ];
         ASSERT_EQ( model.noises.size(), 1 );
         EXPECT_EQ( model.noises[ 0 ].description, QString( "RunB.2B260.e1_a2_2DSA_i01.ti_noise" ) );
      }
}

// A change of investigator since the listing drops it, and loadAll then
// refuses, as it says it does
TEST_F( TestUSDataCatalogMock, AWholeStoreReadAfterAChangeOfInvestigatorIsRefused )
{
   US_DataCatalog catalog;
   attachAndList( catalog );

   InvestigatorGuard other( kInv + 1 );
   server.calls.clear();
   QString error;
   EXPECT_FALSE( catalog.loadAll( error ) );
   EXPECT_FALSE( error.isEmpty() );
   EXPECT_EQ( catalog.runCount(), 0 );
   EXPECT_TRUE( server.calls.isEmpty() ) << server.calls.join( "," ).toStdString();
}

// An ID the server gives as NULL or 0 is "-1", as on disk
TEST_F( TestUSDataCatalogMock, AMissingIdIsMinusOne )
{
   addOrphans();
   server.models << FModel{ "310", "model-10", "Zero.model", "", "0" };

   for ( int tiers : { int( US_DataCatalog::AllTiers ), 0 } )
   {
      SCOPED_TRACE( tiers ? "catalog procedures" : "old procedures" );
      US_DataCatalog catalog;
      catalog.setTiers( tiers );
      attachAndList( catalog );
      QString error;
      ASSERT_TRUE( catalog.loadAll( error ) ) << error.toStdString();

      for ( const US_DataCatalog::Model& model : catalog.orphans().models )
      {
         if ( model.guid == "model-7" || model.guid == "model-10" )
            EXPECT_EQ( model.editID, QString( "-1" ) ) << model.guid.toStdString();
      }
      for ( const US_DataCatalog::Raw& raw : catalog.orphans().raws )
         if ( raw.guid == "raw-9" )
            EXPECT_EQ( raw.solutionID, QString( "-1" ) );
      for ( const US_DataCatalog::Edit& edit : catalog.orphans().edits )
         EXPECT_TRUE( edit.rawID.toInt() > 0 || edit.rawID == "-1" ) << edit.rawID.toStdString();
   }
}
