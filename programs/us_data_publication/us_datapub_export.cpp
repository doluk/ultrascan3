//! \file us_datapub_export.cpp
#include "us_datapub_export.h"
#include "us_datapub_hash.h"
#include "us_datapub_records.h"

#include "us_settings.h"
#include "us_defines.h"
#include "us_util.h"
#include "us_project.h"
#include "us_solution.h"
#include "us_buffer.h"
#include "us_analyte.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_experiment.h"
#include "us_simparms.h"
#include "us_hardware.h"
#include "us_rotor.h"
#include "us_time_state.h"

US_DataPubExporter::Selection::Selection()
{
   fromDb           = false;
   scope            = US_DataPub::ScopeNoise;
   rawsExplicit     = false;
   editsExplicit    = false;
   modelsExplicit   = false;
   noisesExplicit   = false;
   includeTimeState = true;
}

void US_DataPubExporter::Selection::setRaws( const QStringList& guids )
{
   rawGUIDs     = guids;
   rawsExplicit = true;
}

void US_DataPubExporter::Selection::setEdits( const QStringList& guids )
{
   editGUIDs     = guids;
   editsExplicit = true;
}

void US_DataPubExporter::Selection::setModels( const QStringList& guids )
{
   modelGUIDs     = guids;
   modelsExplicit = true;
}

void US_DataPubExporter::Selection::setNoises( const QStringList& guids )
{
   noiseGUIDs     = guids;
   noisesExplicit = true;
}

US_DataPub::Scope US_DataPubExporter::Selection::effectiveScope( void ) const
{
   if ( scope >= US_DataPub::ScopeRawData  &&
        scope <  US_DataPub::ScopeSolutions )
      return US_DataPub::ScopeSolutions;

   return scope;
}

US_DataPubExporter::US_DataPubExporter( QObject* parent ) : QObject( parent )
{
   stage_payloads = true;
   step_count     = 0;
}

US_DataPubExporter::~US_DataPubExporter()
{
   bundle.cleanup();
}

const US_DataPubManifest& US_DataPubExporter::manifest( void ) const
{
   return mani;
}

QStringList US_DataPubExporter::log( void ) const
{
   return logtext;
}

void US_DataPubExporter::cleanup( void )
{
   bundle.cleanup();
}

void US_DataPubExporter::note( const QString& text )
{
   logtext << text;
   emit message( text );
}

bool US_DataPubExporter::copyPayload( const QString& source,
                                      const QString& staged, QString& error )
{
   if ( ! QFile::exists( source ) )
   {
      error = tr( "The payload file %1 does not exist" ).arg( source );
      return false;
   }

   if ( QFile::exists( staged )  &&  ! QFile::remove( staged ) )
   {
      error = tr( "Cannot replace the staged file %1" ).arg( staged );
      return false;
   }

   if ( ! QFile::copy( source, staged ) )
   {
      error = tr( "Cannot copy %1 to %2" ).arg( source ).arg( staged );
      return false;
   }

   error.clear();

   return true;
}

bool US_DataPubExporter::finishEntity( US_DataPubEntity& entity,
                                       const QString& stagedPath )
{
   step_count++;
   emit stepDone( step_count );

   if ( ! stage_payloads )  return true;

   entity.sha256 = US_DataPubHash::fileHash( stagedPath );

   return ! entity.sha256.isEmpty();
}

// --------------------------------------------------------------- entry points

bool US_DataPubExporter::previewManifest( const Selection& selection,
                                          QString& error )
{
   return build( selection, false, error );
}

bool US_DataPubExporter::exportBundle( const Selection& selection,
                                       const QString& bundlePath,
                                       QString& error )
{
   if ( bundlePath.trimmed().isEmpty() )
   {
      error = tr( "No bundle file name was given" );
      return false;
   }

   if ( ! build( selection, true, error ) )  return false;

   QString manifestPath = bundle.stagedPath( US_DataPubBundle::manifestName() );

   if ( ! mani.write( manifestPath, error ) )  return false;

   note( tr( "Packing the bundle into %1" ).arg( bundlePath ) );

   if ( ! bundle.pack( bundlePath, error ) )  return false;

   note( tr( "Wrote %1 records to %2" ).arg( mani.total() ).arg( bundlePath ) );

   bundle.cleanup();

   return true;
}

// --------------------------------------------------------------------- build

bool US_DataPubExporter::build( const Selection& selection, bool payloads,
                                QString& error )
{
   mani.clear();
   logtext.clear();
   cal_guids.clear();
   sol_guids.clear();
   stage_payloads = payloads;
   step_count     = 0;

   US_DataPub::Scope scope = selection.effectiveScope();

   if ( scope == US_DataPub::ScopeNone )
   {
      error = tr( "No export scope was selected" );
      return false;
   }

   if ( scope != selection.scope )
      note( tr( "Scope raised from \"%1\" to \"%2\":"
                " raw data cannot be imported without its solutions" )
            .arg( US_DataPub::scopeKey( selection.scope ) )
            .arg( US_DataPub::scopeKey( scope ) ) );

   if ( ! catalog.open( selection.fromDb, selection.dbPassword, error ) )
      return false;

   if ( payloads  &&  ! bundle.createStaging( error ) )  return false;

   mani.version    = US_DataPubManifest::currentVersion;
   mani.created    = QDateTime::currentDateTimeUtc().toString( Qt::ISODate );
   mani.generator  = QString( "us_data_publication " ) + US_Version;
   mani.source     = selection.fromDb ? QString( "db" ) : QString( "disk" );
   mani.scope      = US_DataPub::scopeKey( scope );
   mani.bundleGUID = US_Util::new_guid();
   mani.comment    = selection.comment;

   QList< US_DataPubCatalog::Run > runs;

   if ( ! gatherRuns( selection, runs, error ) )  return false;

   QList< US_DataPubCatalog::ExpInfo > infos;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      US_DataPubCatalog::ExpInfo info;

      if ( ! catalog.expInfo( runs[ ii ], info, error ) )  return false;

      infos << info;
   }

   // The records are collected in dependency order so that every entity
   // can name the GUIDs it depends on.  The manifest still lists them in
   // the declared section order, which the sections take care of.

   // ---- project -----------------------------------------------------------
   if ( scope >= US_DataPub::ScopeProject )
      if ( ! addProject( selection, infos, error ) )  return false;

   // ---- rotor calibration -------------------------------------------------
   if ( scope >= US_DataPub::ScopeRotorCalibration )
      for ( int ii = 0; ii < infos.size(); ii++ )
         if ( ! addCalibration( infos[ ii ], error ) )  return false;

   // ---- centerpieces ------------------------------------------------------
   if ( scope >= US_DataPub::ScopeCenterpiece )
      for ( int ii = 0; ii < infos.size(); ii++ )
         if ( ! addCenterpieces( infos[ ii ], error ) )  return false;

   // ---- experiment --------------------------------------------------------
   if ( scope >= US_DataPub::ScopeExperiment )
      for ( int ii = 0; ii < runs.size(); ii++ )
         if ( ! addExperiment( runs[ ii ], infos[ ii ], error ) )  return false;

   // ---- solutions, with their buffers and analytes ------------------------
   if ( scope >= US_DataPub::ScopeSolutions )
      for ( int ii = 0; ii < runs.size(); ii++ )
         if ( ! addSolutions( runs[ ii ], infos[ ii ], error ) )  return false;

   // ---- raw data and time state -------------------------------------------
   if ( scope >= US_DataPub::ScopeRawData )
   {
      for ( int ii = 0; ii < runs.size(); ii++ )
      {
         if ( ! addRawData( runs[ ii ], infos[ ii ], error ) )  return false;

         if ( selection.includeTimeState )
            if ( ! addTimeState( runs[ ii ], error ) )  return false;
      }
   }

   // ---- edits -------------------------------------------------------------
   if ( scope >= US_DataPub::ScopeEdits )
      for ( int ii = 0; ii < runs.size(); ii++ )
         if ( ! addEdits( runs[ ii ], error ) )  return false;

   // ---- models ------------------------------------------------------------
   QList< US_DataPubCatalog::Model > models;

   if ( scope >= US_DataPub::ScopeModels )
   {
      QList< US_DataPubCatalog::Model > found = catalog.models( runs, error );

      if ( ! error.isEmpty() )  return false;

      for ( int ii = 0; ii < found.size(); ii++ )
      {
         if ( selection.modelsExplicit  &&
              ! selection.modelGUIDs.contains( found[ ii ].guid ) )  continue;

         models << found[ ii ];
      }

      if ( ! addModels( models, error ) )  return false;
   }

   // ---- noise -------------------------------------------------------------
   if ( scope >= US_DataPub::ScopeNoise  &&  ! models.isEmpty() )
   {
      QList< US_DataPubCatalog::Noise > found = catalog.noises( models, error );

      if ( ! error.isEmpty() )  return false;

      QList< US_DataPubCatalog::Noise > noises;

      for ( int ii = 0; ii < found.size(); ii++ )
      {
         if ( selection.noisesExplicit  &&
              ! selection.noiseGUIDs.contains( found[ ii ].guid ) )  continue;

         noises << found[ ii ];
      }

      if ( ! addNoises( noises, error ) )  return false;
   }

   emit steps( step_count );

   if ( ! mani.validate( error ) )  return false;

   note( tr( "Collected %1 records" ).arg( mani.total() ) );

   return true;
}

bool US_DataPubExporter::gatherRuns( const Selection& selection,
                                     QList< US_DataPubCatalog::Run >& runs,
                                     QString& error )
{
   runs.clear();

   QStringList runIDs = selection.runIDs;

   if ( runIDs.isEmpty()  &&  ! selection.projectGUID.isEmpty()  &&
        selection.scope > US_DataPub::ScopeProject )
   {
      QList< US_DataPubCatalog::Run > found =
            catalog.runs( selection.projectGUID, error );

      if ( ! error.isEmpty() )  return false;

      for ( int ii = 0; ii < found.size(); ii++ )
         runIDs << found[ ii ].runID;
   }

   if ( runIDs.isEmpty()  &&  selection.scope > US_DataPub::ScopeProject )
   {
      error = tr( "No runs were selected for export" );
      return false;
   }

   for ( int ii = 0; ii < runIDs.size(); ii++ )
   {
      US_DataPubCatalog::Run run;

      if ( ! catalog.runByID( runIDs[ ii ], run, error ) )  return false;

      if ( ! catalog.loadRunDetails( run, error ) )         return false;

      // Keep only the selected raw triples and edits
      QList< US_DataPubCatalog::Raw > raws;

      for ( int jj = 0; jj < run.raws.size(); jj++ )
      {
         US_DataPubCatalog::Raw raw = run.raws[ jj ];

         if ( selection.rawsExplicit  &&
              ! selection.rawGUIDs.contains( raw.guid ) )  continue;

         if ( selection.editsExplicit )
         {
            QList< US_DataPubCatalog::Edit > edits;

            for ( int kk = 0; kk < raw.edits.size(); kk++ )
               if ( selection.editGUIDs.contains( raw.edits[ kk ].guid ) )
                  edits << raw.edits[ kk ];

            raw.edits = edits;
         }

         raws << raw;
      }

      run.raws = raws;

      if ( run.raws.isEmpty() )
         note( tr( "Run %1 contributes no raw data to this bundle" )
               .arg( run.runID ) );

      runs << run;
   }

   error.clear();

   return true;
}

// ------------------------------------------------------------------- project

bool US_DataPubExporter::addProject(
      const Selection& selection,
      const QList< US_DataPubCatalog::ExpInfo >& infos, QString& error )
{
   QString guid = selection.projectGUID;
   QString id   = QString( "-1" );

   for ( int ii = 0; ii < infos.size()  &&  guid.isEmpty(); ii++ )
   {
      guid = infos[ ii ].projectGUID;
      id   = infos[ ii ].projectID;
   }

   if ( guid.isEmpty() )
   {
      note( tr( "No project is associated with the selected runs" ) );
      return true;
   }

   for ( int ii = 0; ii < infos.size(); ii++ )
      if ( infos[ ii ].projectGUID == guid )
         id = infos[ ii ].projectID;

   US_Project project;
   QString    lookup = guid;
   int        status;

   if ( catalog.isDb() )
   {
      int projectID = id.toInt();

      if ( projectID < 1 )
      {
         QStringList query;
         query << "get_projectID_from_GUID" << guid;
         catalog.db()->query( query );

         if ( catalog.db()->next() )
            projectID = catalog.db()->value( 0 ).toString().toInt();
      }

      status = project.readFromDB( projectID, catalog.db() );
   }

   else
      status = project.readFromDisk( lookup );

   if ( status != US_DB2::OK )
   {
      error = tr( "The project %1 could not be read (status %2)" )
              .arg( guid ).arg( status );
      return false;
   }

   US_DataPubEntity entity( US_DataPub::Project );
   entity.id      = QString::number( project.projectID );
   entity.guid    = project.projectGUID;
   entity.name    = project.projectDesc;
   entity.payload = US_DataPub::payloadDir( US_DataPub::Project ) + "/"
                    + project.projectGUID + ".xml";

   if ( stage_payloads )
   {
      QString staged = bundle.stagedPath( entity.payload );

      if ( ! project.saveToFile( staged ) )
      {
         error = tr( "The project %1 could not be staged" ).arg( guid );
         return false;
      }

      if ( ! finishEntity( entity, staged ) )
      {
         error = tr( "The staged project payload could not be digested" );
         return false;
      }
   }

   else
      finishEntity( entity, QString() );

   mani.add( entity );
   note( tr( "Project: %1" ).arg( entity.name ) );

   return true;
}

// ---------------------------------------------------------------- experiment

bool US_DataPubExporter::buildTriplesFromDb(
      const US_DataPubCatalog::Run& run,
      QList< US_Convert::TripleInfo >& triples, QString& error )
{
   triples.clear();

   US_DB2*     dbase = catalog.db();
   QStringList query;

   // Cell/channel/centerpiece assignments of the experiment
   QStringList cellNames;
   QStringList chanNames;
   QStringList cpIDs;

   query << "all_cell_experiments" << run.id;
   dbase->query( query );

   while ( dbase->next() )
   {
      QString letters( "SABCDEFGH" );
      int     chanx = qMax( 0, dbase->value( 3 ).toInt() );
      chanx         = qMin( chanx, letters.size() - 1 );

      cellNames << dbase->value( 2 ).toString();
      chanNames << QString( letters[ chanx ] );
      cpIDs     << dbase->value( 4 ).toString();
   }

   int     prevSolID = -1;
   QString prevRunType;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataPubCatalog::Raw& raw = run.raws[ ii ];

      query.clear();
      query << "get_rawData" << raw.id;
      dbase->query( query );

      if ( ! dbase->next() )
      {
         error = tr( "The raw data record %1 could not be read: %2" )
                 .arg( raw.id ).arg( dbase->lastError() );
         return false;
      }

      US_Convert::TripleInfo triple;
      QString uuidc = dbase->value( 0 ).toString();
      US_Util::uuid_parse( uuidc, (unsigned char*)triple.tripleGUID );

      triple.tripleFilename      = dbase->value( 2 ).toString();
      triple.tripleID            = raw.id.toInt();
      triple.solution.solutionID = dbase->value( 5 ).toInt();
      triple.excluded            = false;

      QStringList parts   = triple.tripleFilename.split( "." );

      if ( parts.size() < 6 )
      {
         error = tr( "The raw data file name \"%1\" is not understood" )
                 .arg( triple.tripleFilename );
         return false;
      }

      QString runType     = parts[ 1 ];
      QString cell        = parts[ 2 ];
      QString channel     = parts[ 3 ];
      QString wl          = ( runType == "WA" )
                            ? QString::number( parts[ 4 ].toDouble() / 1000.0 )
                            : parts[ 4 ];
      triple.tripleDesc   = cell + " / " + channel + " / " + wl;
      prevRunType         = runType;

      int cellx = -1;

      for ( int jj = 0; jj < cellNames.size(); jj++ )
      {
         if ( cellNames[ jj ] != cell  ||  chanNames[ jj ] != channel )
            continue;

         cellx = jj;
         break;
      }

      if ( cellx < 0  &&  ! cpIDs.isEmpty() )   // Pre-channel-table data
         cellx = 0;

      triple.centerpiece = ( cellx < 0 ) ? 0 : cpIDs[ cellx ].toInt();

      if ( triple.solution.solutionID != prevSolID )
      {
         triple.solution.readFromDB( triple.solution.solutionID, dbase );
         prevSolID = triple.solution.solutionID;
      }

      else if ( ! triples.isEmpty() )
         triple.solution = triples.last().solution;

      triples << triple;
   }

   error.clear();

   return true;
}

bool US_DataPubExporter::addExperiment( const US_DataPubCatalog::Run& run,
                                        const US_DataPubCatalog::ExpInfo& info,
                                        QString& error )
{
   US_DataPubEntity entity( US_DataPub::Experiment );
   entity.id   = info.expID.isEmpty() ? run.id : info.expID;
   entity.guid = info.expGUID.isEmpty() ? run.guid : info.expGUID;
   entity.name = run.runID;
   entity.attrs.insert( "label",   info.label.isEmpty() ? run.label
                                                        : info.label );
   entity.attrs.insert( "expType", info.expType.isEmpty() ? run.expType
                                                          : info.expType );
   entity.setDepend( US_DataPub::Project, info.projectGUID );
   entity.setDepend( US_DataPub::RotorCalibration,
                     cal_guids.value( info.runID, QString() ) );

   if ( entity.guid.isEmpty() )
   {
      error = tr( "Run %1 has no experiment GUID and cannot be exported" )
              .arg( run.runID );
      return false;
   }

   QString runType = run.runType;

   if ( stage_payloads )
   {
      if ( ! catalog.isDb() )
      {
         QString source = US_DataPubCatalog::expFilePath( run );
         entity.filename = QFileInfo( source ).fileName();
         entity.payload  = US_DataPub::payloadDir( US_DataPub::Experiment )
                           + "/" + entity.filename;

         if ( ! copyPayload( source, bundle.stagedPath( entity.payload ),
                             error ) )
            return false;
      }

      else
      {
         US_Experiment              exper;
         QVector< US_SimulationParameters::SpeedProfile > speedsteps;
         int status = exper.readFromDB( run.runID, catalog.db(), speedsteps );

         if ( status != US_DB2::OK  &&  status != US_DB2::DBERROR )
         {
            error = tr( "The experiment of run %1 could not be read"
                        " (status %2)" ).arg( run.runID ).arg( status );
            return false;
         }

         QList< US_Convert::TripleInfo > triples;

         if ( ! buildTriplesFromDb( run, triples, error ) )  return false;

         runType = QString( exper.opticalSystem );

         if ( runType.isEmpty()  &&  ! run.raws.isEmpty() )
            runType = run.raws[ 0 ].dataType;

         QString dirPath = bundle.stagedPath(
                           US_DataPub::payloadDir( US_DataPub::Experiment )
                           + "/placeholder" );
         dirPath = QFileInfo( dirPath ).absolutePath();

         if ( exper.saveToDisk( triples, runType, run.runID, dirPath + "/",
                                speedsteps ) != US_Convert::OK )
         {
            error = tr( "The experiment XML of run %1 could not be staged" )
                    .arg( run.runID );
            return false;
         }

         entity.filename = run.runID + "." + runType + ".xml";
         entity.payload  = US_DataPub::payloadDir( US_DataPub::Experiment )
                           + "/" + entity.filename;
      }

      if ( ! finishEntity( entity, bundle.stagedPath( entity.payload ) ) )
      {
         error = tr( "The staged experiment payload could not be digested" );
         return false;
      }
   }

   else
   {
      entity.filename = run.runID + "." + runType + ".xml";
      entity.payload  = US_DataPub::payloadDir( US_DataPub::Experiment )
                        + "/" + entity.filename;
      finishEntity( entity, QString() );
   }

   entity.attrs.insert( "runType", runType );

   mani.add( entity );
   note( tr( "Experiment: %1" ).arg( run.runID ) );

   return true;
}

// ------------------------------------------------------------------ raw data

bool US_DataPubExporter::addRawData( const US_DataPubCatalog::Run& run,
                                     const US_DataPubCatalog::ExpInfo& info,
                                     QString& error )
{
   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataPubCatalog::Raw& raw = run.raws[ ii ];
      QString channelKey = raw.triple.section( ".", 0, 0 ) + "/"
                           + raw.triple.section( ".", 1, 1 );
      QString solGUID;

      if ( catalog.isDb() )
      {
         QStringList query;
         query << "get_rawData" << raw.id;
         catalog.db()->query( query );

         if ( catalog.db()->next() )
            solGUID = sol_guids.value( catalog.db()->value( 5 ).toString() );
      }

      else
         solGUID = info.tripleSolutionGUIDs.value( channelKey );

      if ( raw.guid.isEmpty() )
      {
         error = tr( "The raw data file %1 has no GUID and cannot"
                     " be exported" ).arg( raw.filename );
         return false;
      }

      US_DataPubEntity entity( US_DataPub::RawData );
      entity.id       = raw.id;
      entity.guid     = raw.guid;
      entity.filename = raw.filename;
      entity.name     = raw.filename;
      entity.payload  = US_DataPub::payloadDir( US_DataPub::RawData ) + "/"
                        + run.runID + "/" + raw.filename;
      entity.attrs.insert( "runID",    run.runID );
      entity.attrs.insert( "triple",   raw.triple );
      entity.attrs.insert( "dataType", raw.dataType );
      entity.attrs.insert( "centerpieceID",
                           info.tripleCenterpieceIDs.value( channelKey ) );
      entity.setDepend( US_DataPub::Experiment, run.guid );
      entity.setDepend( US_DataPub::Solution, solGUID );

      if ( stage_payloads )
      {
         QString staged = bundle.stagedPath( entity.payload );

         if ( catalog.isDb() )
         {
            int status = catalog.db()->readBlobFromDB( staged,
                         QString( "download_aucData" ), raw.id.toInt() );

            if ( status != US_DB2::OK )
            {
               error = tr( "The raw data record %1 could not be downloaded"
                           " (status %2)" ).arg( raw.filename ).arg( status );
               return false;
            }
         }

         else if ( ! copyPayload( raw.path, staged, error ) )
            return false;

         if ( ! finishEntity( entity, staged ) )
         {
            error = tr( "The staged raw data payload %1 could not be"
                        " digested" ).arg( raw.filename );
            return false;
         }
      }

      else
         finishEntity( entity, QString() );

      mani.add( entity );
   }

   if ( ! run.raws.isEmpty() )
      note( tr( "Raw data: %1 triples of run %2" )
            .arg( run.raws.size() ).arg( run.runID ) );

   return true;
}

// ---------------------------------------------------------------- time state

bool US_DataPubExporter::addTimeState( const US_DataPubCatalog::Run& run,
                                      QString& error )
{
   if ( ! stage_payloads )
   {  // A preview only reports whether a time state exists at all
      bool present = false;

      if ( catalog.isDb() )
      {
         int       tmstID = 0;
         int       expID  = run.id.toInt();
         QString   fname;
         QString   xdefs;
         QString   cksum;
         QDateTime updated;

         US_TimeState::dbExamine( catalog.db(), &tmstID, &expID, &fname,
                                  &xdefs, &cksum, &updated );
         present = ( tmstID > 0 );
      }

      else
      {
         QString dirPath = run.dirPath.isEmpty()
                           ? US_Settings::resultDir() + "/" + run.runID
                           : run.dirPath;
         present = QFile( dirPath + "/" + run.runID
                          + ".time_state.tmst" ).exists();
      }

      if ( ! present )  return true;

      US_DataPubEntity entity( US_DataPub::TimeState );
      entity.id       = run.id;
      entity.guid     = run.guid;
      entity.filename = run.runID + ".time_state.tmst";
      entity.name     = entity.filename;
      entity.payload  = US_DataPub::payloadDir( US_DataPub::TimeState ) + "/"
                        + run.runID + "/" + entity.filename;
      entity.attrs.insert( "runID", run.runID );
      entity.setDepend( US_DataPub::Experiment, run.guid );
      finishEntity( entity, QString() );
      mani.add( entity );

      return true;
   }

   QString workDir = bundle.stagingPath() + "/.tmst";
   QString tmstPath;
   QString xdefPath;

   if ( ! catalog.timeState( run, workDir, tmstPath, xdefPath ) )
      return true;                 // A run without a time state is normal

   US_DataPubEntity entity( US_DataPub::TimeState );
   entity.id       = run.id;
   entity.guid     = run.guid;
   entity.filename = QFileInfo( tmstPath ).fileName();
   entity.name     = entity.filename;
   entity.payload  = US_DataPub::payloadDir( US_DataPub::TimeState ) + "/"
                     + run.runID + "/" + entity.filename;
   entity.attrs.insert( "runID", run.runID );
   entity.setDepend( US_DataPub::Experiment, run.guid );

   QString staged = bundle.stagedPath( entity.payload );

   if ( ! copyPayload( tmstPath, staged, error ) )  return false;

   if ( QFile::exists( xdefPath ) )
   {
      QString defsRel = US_DataPub::payloadDir( US_DataPub::TimeState ) + "/"
                        + run.runID + "/" + QFileInfo( xdefPath ).fileName();
      QString stagedDefs = bundle.stagedPath( defsRel );

      if ( ! copyPayload( xdefPath, stagedDefs, error ) )  return false;

      entity.attrs.insert( "definitionsPayload", defsRel );
      entity.attrs.insert( "definitionsSha256",
                           US_DataPubHash::fileHash( stagedDefs ) );
   }

   if ( ! finishEntity( entity, staged ) )
   {
      error = tr( "The staged time state of run %1 could not be digested" )
              .arg( run.runID );
      return false;
   }

   QDir( workDir ).removeRecursively();

   mani.add( entity );
   note( tr( "Time state: %1" ).arg( run.runID ) );

   return true;
}

// --------------------------------------------------------- rotor calibration

bool US_DataPubExporter::addCalibration(
      const US_DataPubCatalog::ExpInfo& info, QString& error )
{
   int calID = info.calibrationID.toInt();

   if ( calID < 1 )
   {
      note( tr( "Run %1 names no rotor calibration" ).arg( info.runID ) );
      return true;
   }

   US_Rotor::RotorCalibration calibration;
   US_Rotor::Status           status;

   if ( catalog.isDb() )
      status = calibration.readDB( calID, catalog.db() );
   else
      status = calibration.readDisk( calID );

   if ( status != US_Rotor::ROTOR_OK )
   {
      note( tr( "The rotor calibration %1 of run %2 could not be read"
                " and is left out of the bundle" )
            .arg( calID ).arg( info.runID ) );
      return true;
   }

   if ( calibration.GUID.isEmpty() )
      calibration.GUID = info.rotorGUID + QString( "-cal-%1" ).arg( calID );

   cal_guids.insert( info.runID, calibration.GUID );

   if ( mani.contains( US_DataPub::RotorCalibration, calibration.GUID ) )
      return true;

   US_DataPubEntity entity( US_DataPub::RotorCalibration );
   entity.id      = QString::number( calibration.ID );
   entity.guid    = calibration.GUID;
   entity.name    = calibration.label;
   entity.payload = US_DataPub::payloadDir( US_DataPub::RotorCalibration )
                    + "/" + calibration.GUID + ".xml";
   entity.attrs.insert( "rotorGUID",   calibration.rotorGUID );
   entity.attrs.insert( "rotorSerial", info.rotorSerial );
   entity.attrs.insert( "rotorName",   info.rotorName );
   entity.attrs.insert( "coeff1",      QString::number( calibration.coeff1 ) );
   entity.attrs.insert( "coeff2",      QString::number( calibration.coeff2 ) );

   if ( stage_payloads )
   {
      QString staged = bundle.stagedPath( entity.payload );

      if ( ! US_DataPubRecords::writeCalibration( calibration, staged ) )
      {
         error = tr( "The rotor calibration %1 could not be staged" )
                 .arg( calibration.GUID );
         return false;
      }

      if ( ! finishEntity( entity, staged ) )
      {
         error = tr( "The staged rotor calibration could not be digested" );
         return false;
      }
   }

   else
      finishEntity( entity, QString() );

   mani.add( entity );
   note( tr( "Rotor calibration: %1" )
         .arg( entity.name.isEmpty() ? entity.guid : entity.name ) );

   return true;
}

// -------------------------------------------------------------- centerpieces

bool US_DataPubExporter::addCenterpieces(
      const US_DataPubCatalog::ExpInfo& info, QString& error )
{
   if ( info.centerpieceIDs.isEmpty() )  return true;

   QList< US_AbstractCenterpiece > centerpieces;

   if ( ! US_AbstractCenterpiece::read_centerpieces( catalog.db(),
                                                     centerpieces ) )
   {
      note( tr( "The centerpiece table could not be read;"
                " centerpieces are left out of the bundle" ) );
      return true;
   }

   for ( int ii = 0; ii < info.centerpieceIDs.size(); ii++ )
   {
      int serial = info.centerpieceIDs[ ii ].toInt();
      int found  = -1;

      for ( int jj = 0; jj < centerpieces.size(); jj++ )
         if ( centerpieces[ jj ].serial_number == serial )  found = jj;

      if ( found < 0 )
      {
         note( tr( "Centerpiece %1 of run %2 is not in the centerpiece"
                   " table and is left out of the bundle" )
               .arg( serial ).arg( info.runID ) );
         continue;
      }

      const US_AbstractCenterpiece& centerpiece = centerpieces[ found ];
      QString guid = centerpiece.guid.isEmpty()
                     ? QString( "centerpiece-%1" ).arg( serial )
                     : centerpiece.guid;

      if ( mani.contains( US_DataPub::Centerpiece, guid ) )  continue;

      US_DataPubEntity entity( US_DataPub::Centerpiece );
      entity.id      = QString::number( centerpiece.serial_number );
      entity.guid    = guid;
      entity.name    = centerpiece.name;
      entity.payload = US_DataPub::payloadDir( US_DataPub::Centerpiece )
                       + "/centerpiece_" + entity.id + ".xml";
      entity.attrs.insert( "material", centerpiece.material );
      entity.attrs.insert( "channels",
                           QString::number( centerpiece.channels ) );

      if ( stage_payloads )
      {
         QString staged = bundle.stagedPath( entity.payload );

         if ( ! US_DataPubRecords::writeCenterpiece( centerpiece, staged ) )
         {
            error = tr( "The centerpiece %1 could not be staged" ).arg( serial );
            return false;
         }

         if ( ! finishEntity( entity, staged ) )
         {
            error = tr( "The staged centerpiece could not be digested" );
            return false;
         }
      }

      else
         finishEntity( entity, QString() );

      mani.add( entity );
   }

   return true;
}

// ---------------------------------------------- solutions, buffers, analytes

bool US_DataPubExporter::addSolutions( const US_DataPubCatalog::Run& run,
                                       const US_DataPubCatalog::ExpInfo& info,
                                       QString& error )
{
   QStringList refs = catalog.isDb() ? info.solutionIDs : info.solutionGUIDs;

   if ( refs.isEmpty() )
   {
      note( tr( "Run %1 names no solutions" ).arg( run.runID ) );
      return true;
   }

   for ( int ii = 0; ii < refs.size(); ii++ )
   {
      US_Solution solution;
      int         status;

      if ( catalog.isDb() )
         status = solution.readFromDB( refs[ ii ].toInt(), catalog.db() );

      else
      {
         QString guid = refs[ ii ];
         status       = solution.readFromDisk( guid );
      }

      if ( status != US_DB2::OK  &&  status != US_DB2::NO_ANALYTE  &&
           status != US_DB2::NO_BUFFER )
      {
         error = tr( "The solution %1 of run %2 could not be read"
                     " (status %3)" ).arg( refs[ ii ] ).arg( run.runID )
                 .arg( status );
         return false;
      }

      if ( solution.solutionGUID.isEmpty() )
      {
         note( tr( "A solution of run %1 has no GUID and is left out"
                   " of the bundle" ).arg( run.runID ) );
         continue;
      }

      // ---- the buffer of the solution ------------------------------------
      if ( ! solution.buffer.GUID.isEmpty()  &&
           ! mani.contains( US_DataPub::Buffer, solution.buffer.GUID ) )
      {
         US_DataPubEntity entity( US_DataPub::Buffer );
         entity.id      = solution.buffer.bufferID;
         entity.guid    = solution.buffer.GUID;
         entity.name    = solution.buffer.description;
         entity.payload = US_DataPub::payloadDir( US_DataPub::Buffer ) + "/"
                          + solution.buffer.GUID + ".xml";

         if ( stage_payloads )
         {
            QString staged = bundle.stagedPath( entity.payload );

            if ( ! solution.buffer.writeToDisk( staged ) )
            {
               error = tr( "The buffer %1 could not be staged" )
                       .arg( solution.buffer.GUID );
               return false;
            }

            if ( ! finishEntity( entity, staged ) )
            {
               error = tr( "The staged buffer could not be digested" );
               return false;
            }
         }

         else
            finishEntity( entity, QString() );

         mani.add( entity );
      }

      // ---- the analytes of the solution ----------------------------------
      for ( int jj = 0; jj < solution.analyteInfo.size(); jj++ )
      {
         US_Analyte analyte = solution.analyteInfo[ jj ].analyte;

         if ( analyte.analyteGUID.isEmpty() )          continue;
         if ( mani.contains( US_DataPub::Analyte, analyte.analyteGUID ) )
            continue;

         US_DataPubEntity entity( US_DataPub::Analyte );
         entity.id      = analyte.analyteID;
         entity.guid    = analyte.analyteGUID;
         entity.name    = analyte.description;
         entity.payload = US_DataPub::payloadDir( US_DataPub::Analyte ) + "/"
                          + analyte.analyteGUID + ".xml";

         if ( stage_payloads )
         {
            QString staged = bundle.stagedPath( entity.payload );

            if ( analyte.write( false, staged ) != US_DB2::OK )
            {
               error = tr( "The analyte %1 could not be staged" )
                       .arg( analyte.analyteGUID );
               return false;
            }

            if ( ! finishEntity( entity, staged ) )
            {
               error = tr( "The staged analyte could not be digested" );
               return false;
            }
         }

         else
            finishEntity( entity, QString() );

         mani.add( entity );
      }

      // ---- the solution itself -------------------------------------------
      sol_guids.insert( QString::number( solution.solutionID ),
                        solution.solutionGUID );

      if ( mani.contains( US_DataPub::Solution, solution.solutionGUID ) )
         continue;

      US_DataPubEntity entity( US_DataPub::Solution );
      entity.id      = QString::number( solution.solutionID );
      entity.guid    = solution.solutionGUID;
      entity.name    = solution.solutionDesc;
      entity.payload = US_DataPub::payloadDir( US_DataPub::Solution ) + "/"
                       + solution.solutionGUID + ".xml";
      entity.setDepend( US_DataPub::Buffer, solution.buffer.GUID );
      entity.setDepend( US_DataPub::Experiment, run.guid );

      QStringList anaGUIDs;

      for ( int jj = 0; jj < solution.analyteInfo.size(); jj++ )
         anaGUIDs << solution.analyteInfo[ jj ].analyte.analyteGUID;

      if ( ! anaGUIDs.isEmpty() )
         entity.attrs.insert( "analyteGUIDs", anaGUIDs.join( "," ) );

      if ( stage_payloads )
      {
         QString staged = bundle.stagedPath( entity.payload );

         if ( ! solution.saveToFile( staged ) )
         {
            error = tr( "The solution %1 could not be staged" )
                    .arg( solution.solutionGUID );
            return false;
         }

         if ( ! finishEntity( entity, staged ) )
         {
            error = tr( "The staged solution could not be digested" );
            return false;
         }
      }

      else
         finishEntity( entity, QString() );

      mani.add( entity );
      note( tr( "Solution: %1" ).arg( entity.name ) );
   }

   return true;
}

// --------------------------------------------------------------------- edits

bool US_DataPubExporter::addEdits( const US_DataPubCatalog::Run& run,
                                   QString& error )
{
   int knt = 0;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataPubCatalog::Raw& raw = run.raws[ ii ];

      for ( int jj = 0; jj < raw.edits.size(); jj++ )
      {
         const US_DataPubCatalog::Edit& edit = raw.edits[ jj ];

         if ( edit.guid.isEmpty() )
         {
            error = tr( "The edit %1 has no GUID and cannot be exported" )
                    .arg( edit.filename );
            return false;
         }

         US_DataPubEntity entity( US_DataPub::EditedData );
         entity.id       = edit.id;
         entity.guid     = edit.guid;
         entity.filename = edit.filename;
         entity.name     = edit.filename;
         entity.payload  = US_DataPub::payloadDir( US_DataPub::EditedData )
                           + "/" + run.runID + "/" + edit.filename;
         entity.attrs.insert( "runID",  run.runID );
         entity.attrs.insert( "editID", edit.editID );
         entity.attrs.insert( "triple", edit.triple );
         entity.setDepend( US_DataPub::RawData, raw.guid );

         if ( stage_payloads )
         {
            QString staged = bundle.stagedPath( entity.payload );

            if ( catalog.isDb() )
            {
               int status = catalog.db()->readBlobFromDB( staged,
                            QString( "download_editData" ), edit.id.toInt() );

               if ( status != US_DB2::OK )
               {
                  error = tr( "The edit record %1 could not be downloaded"
                              " (status %2)" ).arg( edit.filename )
                          .arg( status );
                  return false;
               }
            }

            else if ( ! copyPayload( edit.path, staged, error ) )
               return false;

            if ( ! finishEntity( entity, staged ) )
            {
               error = tr( "The staged edit payload %1 could not be digested" )
                       .arg( edit.filename );
               return false;
            }
         }

         else
            finishEntity( entity, QString() );

         mani.add( entity );
         knt++;
      }
   }

   if ( knt > 0 )
      note( tr( "Edits: %1 of run %2" ).arg( knt ).arg( run.runID ) );

   return true;
}

// -------------------------------------------------------------------- models

bool US_DataPubExporter::addModels(
      const QList< US_DataPubCatalog::Model >& models, QString& error )
{
   for ( int ii = 0; ii < models.size(); ii++ )
   {
      const US_DataPubCatalog::Model& model = models[ ii ];

      if ( model.guid.isEmpty() )  continue;
      if ( mani.contains( US_DataPub::Model, model.guid ) )  continue;

      if ( ! model.editGUID.isEmpty()  &&
           ! mani.contains( US_DataPub::EditedData, model.editGUID ) )
      {
         error = tr( "Model \"%1\" needs edit %2, which is not part of"
                     " this bundle" ).arg( model.description )
                 .arg( model.editGUID );
         return false;
      }

      US_DataPubEntity entity( US_DataPub::Model );
      entity.id      = model.id;
      entity.guid    = model.guid;
      entity.name    = model.description;
      entity.payload = US_DataPub::payloadDir( US_DataPub::Model ) + "/"
                       + model.guid + ".xml";
      entity.setDepend( US_DataPub::EditedData, model.editGUID );

      if ( ! model.filename.isEmpty() )
         entity.filename = model.filename;

      if ( stage_payloads )
      {
         QString  staged = bundle.stagedPath( entity.payload );
         US_Model record;
         int      status;

         if ( catalog.isDb() )
            status = record.load( model.id, catalog.db() );
         else
            status = record.load( US_Settings::dataDir() + "/models/"
                                  + model.filename );

         if ( status != US_DB2::OK )
         {
            error = tr( "The model \"%1\" could not be read (status %2)" )
                    .arg( model.description ).arg( status );
            return false;
         }

         if ( record.write( staged ) != US_DB2::OK )
         {
            error = tr( "The model \"%1\" could not be staged" )
                    .arg( model.description );
            return false;
         }

         if ( ! finishEntity( entity, staged ) )
         {
            error = tr( "The staged model payload could not be digested" );
            return false;
         }
      }

      else
         finishEntity( entity, QString() );

      mani.add( entity );
   }

   if ( ! models.isEmpty() )
      note( tr( "Models: %1" ).arg( models.size() ) );

   return true;
}

// --------------------------------------------------------------------- noise

bool US_DataPubExporter::addNoises(
      const QList< US_DataPubCatalog::Noise >& noises, QString& error )
{
   for ( int ii = 0; ii < noises.size(); ii++ )
   {
      const US_DataPubCatalog::Noise& noise = noises[ ii ];

      if ( noise.guid.isEmpty() )  continue;
      if ( mani.contains( US_DataPub::Noise, noise.guid ) )  continue;

      if ( ! mani.contains( US_DataPub::Model, noise.modelGUID ) )
      {
         note( tr( "Noise %1 belongs to a model that is not in this"
                   " bundle and is left out" ).arg( noise.guid ) );
         continue;
      }

      US_DataPubEntity entity( US_DataPub::Noise );
      entity.id      = noise.id;
      entity.guid    = noise.guid;
      entity.name    = noise.description;
      entity.payload = US_DataPub::payloadDir( US_DataPub::Noise ) + "/"
                       + noise.guid + ".xml";
      entity.attrs.insert( "noiseType", noise.noiseType );
      entity.setDepend( US_DataPub::Model, noise.modelGUID );

      if ( ! noise.filename.isEmpty() )
         entity.filename = noise.filename;

      if ( stage_payloads )
      {
         QString  staged = bundle.stagedPath( entity.payload );
         US_Noise record;
         int      status;

         if ( catalog.isDb() )
            status = record.load( noise.id, catalog.db() );
         else
            status = record.load( US_Settings::dataDir() + "/noises/"
                                  + noise.filename );

         if ( status != US_DB2::OK )
         {
            error = tr( "The noise record %1 could not be read (status %2)" )
                    .arg( noise.guid ).arg( status );
            return false;
         }

         if ( record.write( staged ) != US_DB2::OK )
         {
            error = tr( "The noise record %1 could not be staged" )
                    .arg( noise.guid );
            return false;
         }

         if ( ! finishEntity( entity, staged ) )
         {
            error = tr( "The staged noise payload could not be digested" );
            return false;
         }
      }

      else
         finishEntity( entity, QString() );

      mani.add( entity );
   }

   if ( ! noises.isEmpty() )
      note( tr( "Noise records: %1" ).arg( noises.size() ) );

   return true;
}
