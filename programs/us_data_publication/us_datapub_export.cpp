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
#include "us_convertio.h"
#include "us_dataIO.h"

namespace
{
   // "cell/channel" of a raw-data triple, the key the experiment XML of a
   // run assigns solutions and centerpieces by
   QString channel_key( const US_DataPubCatalog::Raw& raw )
   {
      return raw.triple.section( ".", 0, 0 ) + "/"
           + raw.triple.section( ".", 1, 1 );
   }

   // A list without repeats, in the order first seen
   QStringList unique( const QStringList& items )
   {
      QStringList out;

      for ( int ii = 0; ii < items.size(); ii++ )
         if ( ! items[ ii ].isEmpty()  &&  ! out.contains( items[ ii ] ) )
            out << items[ ii ];

      return out;
   }
}

US_DataPubExporter::Selection::Selection()
{
   fromDb           = false;
   scope            = US_DataPub::ScopeNoise;
   rawsExplicit     = false;
   editsExplicit    = false;
   modelsExplicit   = false;
   noisesExplicit   = false;
   includeTimeState = true;
   includeProject   = true;
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
   exp_scope      = US_DataPub::ScopeNoise;
   cp_loaded      = false;
   hd_comp_loaded = false;
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

QString US_DataPubExporter::manifestDigestName( void )
{
   return QString( "manifest.sha256" );
}

QString US_DataPubExporter::runTypeOf( const US_DataPubCatalog::Run& run )
{
   if ( ! run.runType.isEmpty() )  return run.runType;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
      if ( ! run.raws[ ii ].dataType.isEmpty() )
         return run.raws[ ii ].dataType;

   return QString( "RA" );
}

void US_DataPubExporter::note( const QString& text )
{
   logtext << text;
   emit message( text );
}

bool US_DataPubExporter::copyPayload( const QString& source,
                                      const QString& staged, QString& error )
{
   if ( staged.isEmpty() )
   {
      error = tr( "The payload of %1 has no place in the bundle" )
              .arg( source );
      return false;
   }

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

/* Count the record and digest its payloads.

   Every payload an entity names -- the main one and every "...Payload"
   attribute -- gets its digest here, so that no extra file can travel
   without one.
*/
bool US_DataPubExporter::finishEntity( US_DataPubEntity& entity,
                                       const QString& stagedPath )
{
   step_count++;
   emit stepDone( step_count );

   if ( ! stage_payloads )  return true;

   entity.sha256 = US_DataPubHash::fileHash( stagedPath );

   if ( entity.sha256.isEmpty() )  return false;

   QStringList keys = entity.payloadKeys();

   for ( int ii = 0; ii < keys.size(); ii++ )
   {
      if ( keys[ ii ] == "payload" )  continue;

      QString path = bundle.stagingPath() + "/" + entity.attrs.value( keys[ ii ] );
      QString hash = US_DataPubHash::fileHash( path );

      if ( hash.isEmpty() )  return false;

      entity.attrs.insert( US_DataPub::digestKeyOf( keys[ ii ] ), hash );
   }

   return true;
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

   if ( ! build( selection, true, error ) )
   {
      bundle.cleanup();
      return false;
   }

   QString manifestPath = bundle.stagedPath( US_DataPubBundle::manifestName() );

   if ( ! mani.write( manifestPath, error ) )
   {
      bundle.cleanup();
      return false;
   }

   // The digest of the manifest itself, in the format sha256sum reads, so
   // that a bundle can be checked with ordinary tools as well
   QString   digest = US_DataPubHash::fileHash( manifestPath );
   QString   digestPath = bundle.stagedPath( manifestDigestName() );
   QSaveFile dfile( digestPath );
   QByteArray line = ( digest + "  " + US_DataPubBundle::manifestName()
                       + "\n" ).toUtf8();

   if ( digest.isEmpty()  ||  ! dfile.open( QIODevice::WriteOnly )  ||
        dfile.write( line ) != line.size()  ||  ! dfile.commit() )
   {
      error = tr( "The manifest digest could not be written" );
      bundle.cleanup();
      return false;
   }

   note( tr( "Packing the bundle into %1" ).arg( bundlePath ) );

   if ( ! bundle.pack( bundlePath, error ) )
   {
      bundle.cleanup();
      return false;
   }

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
   cal_guids    .clear();
   raw_solutions.clear();
   dropped_raws .clear();
   cp_table     .clear();
   hd_components.clear();
   cp_loaded      = false;
   hd_comp_loaded = false;
   stage_payloads = payloads;
   step_count     = 0;

   US_DataPub::Scope scope = selection.effectiveScope();
   exp_scope       = scope;
   explicit_noises = selection.noisesExplicit ? selection.noiseGUIDs
                                              : QStringList();

   if ( scope == US_DataPub::ScopeNone )
   {
      error = tr( "No export scope was selected" );
      return false;
   }

   if ( scope == US_DataPub::ScopeProject  &&  ! selection.includeProject )
   {
      error = tr( "The scope is the project alone, but the project is not"
                  " being exported: there would be nothing in the bundle" );
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

   // The sections this export fills in, so that the manifest can tell an
   // exported but empty section from one that was not exported at all
   QList< US_DataPub::EntityType > types = US_DataPubManifest::orderedTypes();

   for ( int ii = 0; ii < types.size(); ii++ )
   {
      US_DataPub::EntityType type = types[ ii ];

      if ( type == US_DataPub::Project  &&  ! selection.includeProject )
         continue;

      if ( type == US_DataPub::TimeState  &&  ! selection.includeTimeState )
         continue;

      if ( US_DataPub::typeScope( type ) <= scope )
         mani.declareSection( type );
   }

   QList< US_DataPubCatalog::Run > runs;

   if ( ! gatherRuns( selection, runs, error ) )  return false;

   QList< US_DataPubCatalog::ExpInfo > infos;

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      US_DataPubCatalog::ExpInfo info;

      if ( ! catalog.expInfo( runs[ ii ], info, error ) )  return false;

      infos << info;
   }

   // The models are known before anything is staged, so the number of
   // steps can be announced up front
   QList< US_DataPubCatalog::Model > models;

   if ( scope >= US_DataPub::ScopeModels )
   {
      if ( ! collectModels( selection, runs, models, error ) )  return false;
   }

   else if ( selection.modelsExplicit  &&  ! selection.modelGUIDs.isEmpty() )
      note( tr( "%1 selected model(s) are left out: the scope ends at \"%2\"" )
            .arg( selection.modelGUIDs.size() )
            .arg( US_DataPub::scopeKey( scope ) ) );

   int estimate = runs.size() + models.size()
                  + ( selection.includeProject ? 1 : 0 );

   for ( int ii = 0; ii < runs.size(); ii++ )
   {
      if ( scope >= US_DataPub::ScopeRawData )
         estimate += runs[ ii ].raws.size() * 2      // raw data, solution
                     + ( selection.includeTimeState ? 1 : 0 ) + 2;

      for ( int jj = 0; jj < runs[ ii ].raws.size(); jj++ )
         if ( scope >= US_DataPub::ScopeEdits )
            estimate += runs[ ii ].raws[ jj ].edits.size();
   }

   emit steps( estimate );

   // The records are collected in dependency order so that every entity
   // can name the GUIDs it depends on.

   // ---- project -----------------------------------------------------------
   if ( selection.includeProject )
   {
      if ( ! addProjects( selection, infos, error ) )  return false;
   }

   else
      note( tr( "The project is left out of the bundle; the runs still name"
                " it, so an import can attach them to a project the target"
                " already has" ) );

   // ---- rotor calibration -------------------------------------------------
   if ( scope >= US_DataPub::ScopeRotorCalibration )
      for ( int ii = 0; ii < infos.size(); ii++ )
         if ( ! addCalibration( infos[ ii ], error ) )  return false;

   // ---- centerpieces ------------------------------------------------------
   if ( scope >= US_DataPub::ScopeCenterpiece )
      for ( int ii = 0; ii < runs.size(); ii++ )
         if ( ! addCenterpieces( runs[ ii ], infos[ ii ], error ) )
            return false;

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
   if ( scope >= US_DataPub::ScopeModels )
      if ( ! addModels( models, error ) )  return false;

   // ---- noise -------------------------------------------------------------
   if ( scope >= US_DataPub::ScopeNoise )
   {
      QList< US_DataPubCatalog::Noise > noises;

      if ( ! collectNoises( selection, models, noises, error ) )  return false;

      if ( ! addNoises( noises, error ) )  return false;
   }

   else if ( selection.noisesExplicit  &&  ! selection.noiseGUIDs.isEmpty() )
      note( tr( "%1 selected noise record(s) are left out: the scope ends at"
                " \"%2\"" ).arg( selection.noiseGUIDs.size() )
            .arg( US_DataPub::scopeKey( scope ) ) );

   emit steps( step_count );

   if ( mani.total() < 1 )
   {
      error = tr( "There is nothing to export: the selection holds no record"
                  " within the scope \"%1\"" )
              .arg( US_DataPub::scopeKey( scope ) );
      return false;
   }

   // Raw data cannot be re-created without the solution in its channel
   QList< US_DataPubEntity > raws = mani.section( US_DataPub::RawData );

   for ( int ii = 0; ii < raws.size(); ii++ )
   {
      if ( ! raws[ ii ].depend( US_DataPub::Solution ).isEmpty() )  continue;

      error = tr( "The raw data %1 has no solution in the bundle" )
              .arg( raws[ ii ].filename );
      return false;
   }

   if ( ! mani.validate( error ) )  return false;

   note( tr( "Collected %1 records" ).arg( mani.total() ) );

   return true;
}

bool US_DataPubExporter::gatherRuns( const Selection& selection,
                                     QList< US_DataPubCatalog::Run >& runs,
                                     QString& error )
{
   runs.clear();

   QStringList runIDs = unique( selection.runIDs );
   US_DataPub::Scope scope = selection.effectiveScope();

   if ( runIDs.isEmpty()  &&  ! selection.projectGUID.isEmpty()  &&
        scope > US_DataPub::ScopeProject )
   {
      QList< US_DataPubCatalog::Run > found =
            catalog.runs( selection.projectGUID, error );

      if ( ! error.isEmpty() )  return false;

      for ( int ii = 0; ii < found.size(); ii++ )
         runIDs << found[ ii ].runID;
   }

   if ( runIDs.isEmpty()  &&  scope > US_DataPub::ScopeProject )
   {
      error = tr( "No runs were selected for export" );
      return false;
   }

   QStringList seenRaws;
   QStringList seenEdits;
   int         nraws = 0;

   for ( int ii = 0; ii < runIDs.size(); ii++ )
   {
      // The run ID names directories and files in the bundle
      if ( ! US_DataPub::runIdIsUsable( runIDs[ ii ] ) )
      {
         error = tr( "\"%1\" cannot be exported: a run ID may hold only"
                     " letters, digits, '_' and '-', at most 80 of them" )
                 .arg( runIDs[ ii ] );
         return false;
      }

      US_DataPubCatalog::Run run;

      if ( ! catalog.runByID( runIDs[ ii ], run, error ) )  return false;

      if ( ! catalog.loadRunDetails( run, error ) )         return false;

      if ( ! selection.projectGUID.isEmpty()  &&
           run.projectGUID != selection.projectGUID )
      {
         error = tr( "Run %1 belongs to project %2, not to the project %3"
                     " the export was asked for" )
                 .arg( run.runID )
                 .arg( run.projectGUID.isEmpty() ? tr( "(none)" )
                                                 : run.projectGUID )
                 .arg( selection.projectGUID );
         return false;
      }

      // Keep only the selected raw triples and edits
      QList< US_DataPubCatalog::Raw > raws;
      QStringList                     dropped;

      for ( int jj = 0; jj < run.raws.size(); jj++ )
      {
         US_DataPubCatalog::Raw raw = run.raws[ jj ];

         for ( int kk = 0; kk < raw.edits.size(); kk++ )
            seenEdits << raw.edits[ kk ].guid;

         seenRaws << raw.guid;

         if ( selection.rawsExplicit  &&
              ! selection.rawGUIDs.contains( raw.guid ) )
         {
            dropped << raw.guid;
            continue;
         }

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
      nraws   += raws.size();
      dropped_raws.insert( run.runID, dropped );

      if ( run.raws.isEmpty() )
         note( tr( "Warning: run %1 contributes no raw data to this bundle" )
               .arg( run.runID ) );

      runs << run;
   }

   // A GUID that was asked for by name and matches nothing is a mistake
   // in the request, not something to pass over
   QStringList unknown;

   if ( selection.rawsExplicit )
      for ( int ii = 0; ii < selection.rawGUIDs.size(); ii++ )
         if ( ! seenRaws.contains( selection.rawGUIDs[ ii ] ) )
            unknown << selection.rawGUIDs[ ii ];

   if ( ! unknown.isEmpty() )
   {
      error = tr( "No raw data of the selected runs has the GUID %1" )
              .arg( unknown.join( ", " ) );
      return false;
   }

   if ( selection.editsExplicit  &&  scope >= US_DataPub::ScopeEdits )
      for ( int ii = 0; ii < selection.editGUIDs.size(); ii++ )
         if ( ! seenEdits.contains( selection.editGUIDs[ ii ] ) )
            unknown << selection.editGUIDs[ ii ];

   if ( ! unknown.isEmpty() )
   {
      error = tr( "No edit of the selected runs has the GUID %1" )
              .arg( unknown.join( ", " ) );
      return false;
   }

   if ( scope >= US_DataPub::ScopeRawData  &&  ! runs.isEmpty()  &&
        nraws == 0 )
   {
      error = tr( "None of the selected runs has raw data to export" );
      return false;
   }

   error.clear();

   return true;
}

// ------------------------------------------------------------------- project

/* Every project the runs belong to travels with them.  A selection names
   at most one -- and gatherRuns() has made sure the runs belong to it --
   but runs picked one by one may come from several.
*/
bool US_DataPubExporter::addProjects(
      const Selection& selection,
      const QList< US_DataPubCatalog::ExpInfo >& infos, QString& error )
{
   QStringList guids;
   QStringList ids;

   if ( ! selection.projectGUID.isEmpty() )
   {
      guids << selection.projectGUID;
      ids   << QString( "-1" );
   }

   for ( int ii = 0; ii < infos.size(); ii++ )
   {
      QString guid = infos[ ii ].projectGUID;
      int     at   = guids.indexOf( guid );

      if ( guid.isEmpty() )  continue;

      if ( at < 0 )
      {
         guids << guid;
         ids   << infos[ ii ].projectID;
      }

      else if ( ids[ at ].toInt() < 1 )
         ids[ at ] = infos[ ii ].projectID;
   }

   if ( guids.isEmpty() )
   {
      note( tr( "No project is associated with the selected runs" ) );
      return true;
   }

   if ( guids.size() > 1 )
      note( tr( "The selected runs belong to %1 projects; all of them are"
                " exported" ).arg( guids.size() ) );

   for ( int ii = 0; ii < guids.size(); ii++ )
      if ( ! addProject( guids[ ii ], ids[ ii ], error ) )  return false;

   return true;
}

bool US_DataPubExporter::addProject( const QString& guid, const QString& id,
                                     QString& error )
{
   if ( mani.contains( US_DataPub::Project, guid ) )  return true;

   US_Project project;
   QString    lookup = guid;
   int        status;

   if ( catalog.isDb() )
   {
      IUS_DB2* dbase     = catalog.db();
      int      projectID = id.toInt();

      if ( projectID < 1 )
      {
         QStringList query;
         query << "get_projectID_from_GUID" << guid;
         dbase->query( query );

         if ( dbase->lastErrno() != IUS_DB2::OK  &&
              dbase->lastErrno() != IUS_DB2::NOROWS )
         {
            error = tr( "The project %1 could not be looked up: %2" )
                    .arg( guid ).arg( dbase->lastError() );
            return false;
         }

         if ( dbase->next() )
            projectID = dbase->value( 0 ).toString().toInt();
      }

      if ( projectID < 1 )
      {
         error = tr( "The project %1 is not in the database" ).arg( guid );
         return false;
      }

      status = project.readFromDB( projectID, dbase );
   }

   else
      status = project.readFromDisk( lookup );

   if ( status != IUS_DB2::OK  ||  project.projectGUID != guid )
   {
      error = tr( "The project %1 could not be read (status %2)" )
              .arg( guid ).arg( status );

      if ( ! catalog.isDb() )
         error += tr( "; no project file in %1 has that GUID.  To export"
                      " the runs without their project, leave the project"
                      " out (--no-project on the command line)" )
                  .arg( US_Settings::dataDir() + "/projects" );

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

      if ( staged.isEmpty()  ||  ! project.saveToFile( staged ) )
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

/* The triples the experiment XML of a database run describes.

   This is what US_ConvertIO's raw-data reader builds before it writes the
   XML, without downloading any data into the results directory: the
   catalog already knows each triple's GUID, file name and solution.
*/
bool US_DataPubExporter::buildTriplesFromDb(
      const US_DataPubCatalog::Run& run,
      QList< US_Convert::TripleInfo >& triples, QString& error )
{
   triples.clear();

   IUS_DB2*      dbase   = catalog.db();
   const QString letters = US_Util::channel_letters();
   QStringList   query;

   // Cell/channel/centerpiece assignments of the experiment
   QStringList cellNames;
   QStringList chanNames;
   QStringList cpIDs;

   query << "all_cell_experiments" << run.id;
   dbase->query( query );

   if ( dbase->lastErrno() != IUS_DB2::OK  &&
        dbase->lastErrno() != IUS_DB2::NOROWS )
   {
      error = tr( "The cell assignments of run %1 could not be read: %2" )
              .arg( run.runID ).arg( dbase->lastError() );
      return false;
   }

   while ( dbase->next() )
   {
      int chanx = qBound( 0, dbase->value( 3 ).toInt(), letters.size() - 1 );

      cellNames << dbase->value( 2 ).toString();
      chanNames << QString( letters[ chanx ] );
      cpIDs     << dbase->value( 4 ).toString();
   }

   QMap< int, US_Solution > solutions;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataPubCatalog::Raw& raw = run.raws[ ii ];

      US_Convert::TripleInfo triple;
      US_Util::uuid_parse( raw.guid, (unsigned char*)triple.tripleGUID );

      triple.tripleFilename = raw.filename;
      triple.tripleID       = raw.id.toInt();
      triple.excluded       = false;

      QStringList parts = triple.tripleFilename.split( "." );

      if ( parts.size() < 6 )
      {
         error = tr( "The raw data file name \"%1\" is not understood" )
                 .arg( triple.tripleFilename );
         return false;
      }

      QString runType   = parts[ 1 ];
      QString cell      = parts[ 2 ];
      QString channel   = parts[ 3 ];
      QString wl        = ( runType == "WA" )
                          ? QString::number( parts[ 4 ].toDouble() / 1000.0 )
                          : parts[ 4 ];
      triple.tripleDesc = cell + " / " + channel + " / " + wl;

      int cellx = -1;

      for ( int jj = 0; jj < cellNames.size(); jj++ )
      {
         if ( cellNames[ jj ] != cell  ||  chanNames[ jj ] != channel )
            continue;

         cellx = jj;
         break;
      }

      // A channel the cell table does not describe has no centerpiece; it
      // is not given some other channel's
      if ( cellx < 0 )
         note( tr( "Run %1 records no centerpiece for cell %2, channel %3" )
               .arg( run.runID ).arg( cell ).arg( channel ) );

      triple.centerpiece = ( cellx < 0 ) ? 0 : cpIDs[ cellx ].toInt();

      // The solutions are only needed when they travel too; an export of
      // the experiment alone names what it can and goes on without them
      bool needed = ( exp_scope >= US_DataPub::ScopeSolutions );
      int  solID  = raw.solutionID.toInt();

      if ( solID < 1 )
      {
         if ( needed )
         {
            error = tr( "The raw data %1 names no solution" )
                    .arg( raw.filename );
            return false;
         }

         note( tr( "Warning: the raw data %1 names no solution; the"
                   " experiment XML names none for it" ).arg( raw.filename ) );
      }

      else if ( ! solutions.contains( solID ) )
      {
         US_Solution solution;
         int status = solution.readFromDB( solID, dbase );

         if ( status != IUS_DB2::OK )
         {
            QString why = tr( "The solution %1 of %2 could not be read: %3" )
                          .arg( solID ).arg( raw.filename )
                          .arg( US_Solution::readErrorText( status,
                                dbase->lastError() ) );

            if ( needed )
            {
               error = why;
               return false;
            }

            note( tr( "Warning: %1; the experiment XML names no solution"
                      " for it" ).arg( why ) );
            solution = US_Solution();
         }

         solutions.insert( solID, solution );
      }

      if ( solID > 0 )
         triple.solution = solutions.value( solID );

      triples << triple;
   }

   error.clear();

   return true;
}

/* Read the experiment record of a database run.

   Only read: the RI intensity profile is not cached in the user's results
   directory, as an interactive read would; the export stages it from the
   record itself.
*/
bool US_DataPubExporter::readExperimentDb(
      const US_DataPubCatalog::Run& run, US_Experiment& exper,
      QVector< US_SimulationParameters::SpeedProfile >& speedsteps,
      QString& error )
{
   int status = exper.readFromDB( run.runID, catalog.db(), speedsteps,
                                  QString(), false );

   if ( status != IUS_DB2::OK )
   {
      error = tr( "The experiment of run %1 could not be read (status %2): %3" )
              .arg( run.runID ).arg( status )
              .arg( catalog.db()->lastError() );
      return false;
   }

   return true;
}

/* Stage an RI intensity profile, when it is one an import can read.

   An import refuses a bundle whose profile does not parse, so a profile
   that is empty or broken is left out with a warning rather than carried.
*/
bool US_DataPubExporter::stageRiProfile( const US_DataPubCatalog::Run& run,
                                         const QByteArray& xml,
                                         const QString& rel, QString& error )
{
   QString why;

   if ( ! US_DataPubRecords::riProfileIsUsable( xml, &why ) )
   {
      note( tr( "Warning: the RI intensity profile of run %1 is left out:"
                " %2" ).arg( run.runID ).arg( why ) );
      error.clear();
      return false;
   }

   QString   staged = bundle.stagedPath( rel );
   QSaveFile file( staged );

   if ( staged.isEmpty()  ||  ! file.open( QIODevice::WriteOnly )  ||
        file.write( xml ) != xml.size()  ||  ! file.commit() )
   {
      error = tr( "The RI intensity profile of run %1 could not be staged" )
              .arg( run.runID );
      return false;
   }

   error.clear();

   return true;
}

/* Stage the experiment XML of a disk run.

   The XML names a dataset for every triple the run was converted with,
   but what the bundle carries is the triples that are on disk now and
   were selected: run.raws.  A dataset naming any other triple -- one that
   was deselected, or whose .auc file is gone -- is left out, so that the
   bundle does not name triples, and their solutions, it does not carry.
   When nothing has to go, the file is copied as it is.
*/
bool US_DataPubExporter::stageExperimentXml(
      const US_DataPubCatalog::Run& run, const QString& source,
      const QString& staged, QString& error )
{
   QSet< QString > kept;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
      kept.insert( run.raws[ ii ].guid );

   QStringList dropped = dropped_raws.value( run.runID );

   QFile input( source );

   if ( ! input.open( QIODevice::ReadOnly ) )
   {
      error = tr( "The experiment XML file %1 cannot be read" ).arg( source );
      return false;
   }

   // ---- which datasets go ---------------------------------------------
   QSet< QString > leave;       // GUIDs of the datasets left out
   int             noGuid = 0;  // Datasets that name no GUID

   {
      QXmlStreamReader xml( &input );

      while ( ! xml.atEnd() )
      {
         xml.readNext();

         if ( ! xml.isStartElement()  ||  xml.name().toString() != "dataset" )
            continue;

         QString guid = xml.attributes().value( "guid" ).toString();

         if ( guid.isEmpty() )
            noGuid++;
         else if ( ! kept.contains( guid ) )
            leave.insert( guid );
      }

      if ( xml.hasError() )
      {
         error = tr( "The experiment XML file %1 is not well-formed: %2" )
                 .arg( source ).arg( xml.errorString() );
         return false;
      }
   }

   // A dataset without a GUID cannot be told apart from the others, so
   // the deselected triples cannot be taken out of such a file
   if ( noGuid > 0  &&  ! dropped.isEmpty() )
   {
      error = tr( "The experiment XML of run %1 does not describe every"
                  " triple by its GUID, so the deselected ones cannot be left"
                  " out of it; export all triples of the run instead" )
              .arg( run.runID );
      return false;
   }

   if ( leave.isEmpty() )
   {
      input.close();
      return copyPayload( source, staged, error );
   }

   // ---- copy everything else --------------------------------------------
   input.seek( 0 );

   QSaveFile output( staged );

   if ( ! output.open( QIODevice::WriteOnly ) )
   {
      error = tr( "Cannot stage the experiment XML of run %1" )
              .arg( run.runID );
      return false;
   }

   QXmlStreamReader xml( &input );
   QXmlStreamWriter out( &output );
   out.setAutoFormatting( true );

   int skipDepth = 0;

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( skipDepth > 0 )
      {
         if ( xml.isStartElement() )  skipDepth++;
         if ( xml.isEndElement() )    skipDepth--;
         continue;
      }

      if ( xml.isStartElement()  &&  xml.name().toString() == "dataset"  &&
           leave.contains( xml.attributes().value( "guid" ).toString() ) )
      {
         skipDepth = 1;
         continue;
      }

      // Whitespace is re-made by the writer's own formatting
      if ( xml.isWhitespace() )  continue;

      out.writeCurrentToken( xml );
   }

   input.close();

   if ( xml.hasError()  ||  out.hasError() )
   {
      output.cancelWriting();
      error = tr( "The experiment XML of run %1 could not be staged" )
              .arg( run.runID );
      return false;
   }

   if ( ! output.commit() )
   {
      error = tr( "Cannot stage the experiment XML of run %1" )
              .arg( run.runID );
      return false;
   }

   int missing = 0;

   for ( auto it = leave.constBegin(); it != leave.constEnd(); ++it )
      if ( ! dropped.contains( *it ) )  missing++;

   if ( leave.size() > missing )
      note( tr( "Run %1: %2 deselected triple(s) left out of the experiment"
                " XML" ).arg( run.runID ).arg( leave.size() - missing ) );

   if ( missing > 0 )
      note( tr( "Warning: run %1: %2 triple(s) the experiment XML names have"
                " no raw data file and are left out of it" )
            .arg( run.runID ).arg( missing ) );

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
   if ( mani.contains( US_DataPub::Project, info.projectGUID ) )
      entity.setDepend( US_DataPub::Project, info.projectGUID );

   else if ( ! info.projectGUID.isEmpty() )
   {  // The project is not in the bundle, so it is not a dependency the
      // import can resolve; it is recorded for information instead.
      entity.attrs.insert( "projectGUID", info.projectGUID );
      entity.attrs.insert( "projectDescription", info.projectDesc );
   }

   entity.setDepend( US_DataPub::RotorCalibration,
                     cal_guids.value( info.runID, QString() ) );

   if ( entity.guid.isEmpty() )
   {
      error = tr( "Run %1 has no experiment GUID and cannot be exported" )
              .arg( run.runID );
      return false;
   }

   QString runType = runTypeOf( run );
   QString expDir  = US_DataPub::payloadDir( US_DataPub::Experiment );
   QString ripName = run.runID + ".RIProfile.xml";

   entity.filename = run.runID + "." + runType + ".xml";
   entity.payload  = expDir + "/" + entity.filename;
   entity.attrs.insert( "runType", runType );

   if ( ! stage_payloads )
   {  // A preview names the RI profile when it can see one without reading
      if ( ! catalog.isDb()  &&  ! run.dirPath.isEmpty()  &&
           QFile::exists( run.dirPath + "/" + ripName ) )
         entity.attrs.insert( "riProfilePayload", expDir + "/" + ripName );

      finishEntity( entity, QString() );
      mani.add( entity );
      return true;
   }

   QString staged = bundle.stagedPath( entity.payload );

   if ( staged.isEmpty() )
   {
      error = tr( "Run %1 cannot be staged" ).arg( run.runID );
      return false;
   }

   if ( ! catalog.isDb() )
   {
      QString source = US_DataPubCatalog::expFilePath( run );

      if ( QFileInfo( source ).fileName() != entity.filename )
         source = run.dirPath + "/" + entity.filename;

      if ( ! stageExperimentXml( run, source, staged, error ) )
         return false;

      // The RI intensity profile an RI run is reduced with
      QString ripSource = run.dirPath + "/" + ripName;
      QFile   ripFile( ripSource );

      if ( ripFile.exists() )
      {
         QString    ripRel = expDir + "/" + ripName;
         QByteArray xml;

         if ( ripFile.size() > US_DataPubManifest::maxManifestBytes  ||
              ! ripFile.open( QIODevice::ReadOnly ) )
            note( tr( "Warning: the RI intensity profile %1 cannot be read"
                      " and is left out" ).arg( ripSource ) );

         else
         {
            xml = ripFile.readAll();
            ripFile.close();

            if ( stageRiProfile( run, xml, ripRel, error ) )
               entity.attrs.insert( "riProfilePayload", ripRel );

            else if ( ! error.isEmpty() )
               return false;
         }
      }

      else if ( runType == "RI" )
         note( tr( "Run %1 is RI data without an intensity profile" )
               .arg( run.runID ) );
   }

   else
   {
      US_Experiment              exper;
      QVector< US_SimulationParameters::SpeedProfile > speedsteps;

      if ( ! readExperimentDb( run, exper, speedsteps, error ) )  return false;

      if ( ! exper.expGUID.isEmpty()  &&  exper.expGUID != entity.guid )
      {
         error = tr( "The experiment record of run %1 has the GUID %2, the"
                     " catalog %3" ).arg( run.runID ).arg( exper.expGUID )
                 .arg( entity.guid );
         return false;
      }

      if ( QString( exper.opticalSystem ) != runType  &&
           ! QString( exper.opticalSystem ).isEmpty() )
         note( tr( "Run %1: the experiment record says optical system %2,"
                   " the data files %3; the files are followed" )
               .arg( run.runID ).arg( QString( exper.opticalSystem ) )
               .arg( runType ) );

      QList< US_Convert::TripleInfo > triples;

      if ( ! buildTriplesFromDb( run, triples, error ) )  return false;

      QString dirPath = QFileInfo( staged ).absolutePath();

      if ( exper.saveToDisk( triples, runType, run.runID, dirPath + "/",
                             speedsteps ) != US_Convert::OK  ||
           ! QFile::exists( staged ) )
      {
         error = tr( "The experiment XML of run %1 could not be staged" )
                 .arg( run.runID );
         return false;
      }

      if ( exper.expGUID != entity.guid )
      {
         error = tr( "The experiment GUID of run %1 is not a valid GUID" )
                 .arg( run.runID );
         return false;
      }

      // The RI intensity profile is part of the experiment record in the
      // database; it travels as a file of its own, as on disk
      if ( QString( exper.opticalSystem ) == "RI"  &&
           ! exper.RIProfile.isEmpty() )
      {
         QString ripRel = expDir + "/" + ripName;

         if ( stageRiProfile( run, exper.riProfileXml(), ripRel, error ) )
            entity.attrs.insert( "riProfilePayload", ripRel );

         else if ( ! error.isEmpty() )
            return false;
      }

      else if ( QString( exper.opticalSystem ) == "RI" )
         note( tr( "Run %1 is RI data without an intensity profile" )
               .arg( run.runID ) );
   }

   if ( ! finishEntity( entity, staged ) )
   {
      error = tr( "The staged experiment payload could not be digested" );
      return false;
   }

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
      QString channelKey = channel_key( raw );
      QString solGUID    = raw_solutions.value( raw.guid );

      if ( raw.guid.isEmpty() )
      {
         error = tr( "The raw data file %1 has no GUID and cannot"
                     " be exported" ).arg( raw.filename );
         return false;
      }

      if ( solGUID.isEmpty() )
      {
         error = tr( "The raw data %1 has no solution in the bundle" )
                 .arg( raw.filename );
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

         if ( staged.isEmpty() )
         {
            error = tr( "The raw data %1 cannot be staged" ).arg( raw.filename );
            return false;
         }

         if ( catalog.isDb() )
         {
            QString why;
            int     status = US_ConvertIO::downloadRawDataBlob(
                             catalog.db(), raw.id.toInt(), staged, why );

            if ( status != IUS_DB2::OK )
            {
               error = tr( "The raw data record %1 could not be downloaded"
                           " (status %2): %3" ).arg( raw.filename )
                       .arg( status ).arg( why );
               return false;
            }
         }

         else if ( ! copyPayload( raw.path, staged, error ) )
            return false;

         // An import refuses a payload whose GUID is not its entry's, so
         // a record that does not hold what the catalog says stops here
         if ( ! checkRawGuid( staged, raw.guid, error ) )
         {
            error = tr( "The raw data %1: %2" ).arg( raw.filename ).arg( error );
            return false;
         }

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

/* The time state of a run is a record of its own, with an identity of its
   own: a GUID derived from the experiment's, so that exporting the run
   twice gives it the same one.
*/
bool US_DataPubExporter::addTimeState( const US_DataPubCatalog::Run& run,
                                      QString& error )
{
   QString workDir = bundle.workPath() + "/timestate/" + run.runID;
   QString tmstPath;
   QString xdefPath;
   QString tmstID;

   US_DataPubCatalog::TimeStateStatus status =
      catalog.timeState( run, workDir, stage_payloads, tmstPath, xdefPath,
                         tmstID, error );

   if ( status == US_DataPubCatalog::TimeStateNone )
   {
      error.clear();
      return true;                 // A run without a time state is normal
   }

   if ( status == US_DataPubCatalog::TimeStateFailed )  return false;

   QString dir = US_DataPub::payloadDir( US_DataPub::TimeState ) + "/"
                 + run.runID + "/";

   US_DataPubEntity entity( US_DataPub::TimeState );
   entity.id       = tmstID;
   entity.guid     = US_DataPub::derivedGuid( "timeState:" + run.guid );
   entity.filename = QFileInfo( tmstPath ).fileName();
   entity.name     = entity.filename;
   entity.payload  = dir + entity.filename;
   entity.attrs.insert( "runID", run.runID );
   entity.attrs.insert( "definitionsPayload",
                        dir + QFileInfo( xdefPath ).fileName() );
   entity.setDepend( US_DataPub::Experiment, run.guid );

   if ( stage_payloads )
   {
      QString staged = bundle.stagedPath( entity.payload );

      if ( ! copyPayload( tmstPath, staged, error ) )  return false;

      if ( ! copyPayload( xdefPath, bundle.stagedPath(
                          entity.attrs.value( "definitionsPayload" ) ), error ) )
         return false;

      if ( ! finishEntity( entity, staged ) )
      {
         error = tr( "The staged time state of run %1 could not be digested" )
                 .arg( run.runID );
         return false;
      }

      QDir( workDir ).removeRecursively();
   }

   else
      finishEntity( entity, QString() );

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
      note( tr( "Warning: run %1 names no rotor calibration, so none is"
                " exported with it" ).arg( info.runID ) );
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
      error = tr( "The rotor calibration %1 of run %2 could not be read"
                  " (status %3)" ).arg( calID ).arg( info.runID )
              .arg( int( status ) );
      return false;
   }

   // An old calibration without a GUID gets one derived from what it is,
   // the same every time it is exported
   if ( calibration.rotorGUID.isEmpty() )
      calibration.rotorGUID = info.rotorGUID;

   calibration.GUID = US_DataPubRecords::calibrationGuid( calibration );

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
   entity.attrs.insert( "coeff1",
                        QString::number( calibration.coeff1, 'g', 17 ) );
   entity.attrs.insert( "coeff2",
                        QString::number( calibration.coeff2, 'g', 17 ) );

   if ( stage_payloads )
   {
      QString staged = bundle.stagedPath( entity.payload );

      if ( staged.isEmpty()  ||
           ! US_DataPubRecords::writeCalibration( calibration, staged ) )
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

/* The centerpieces of the triples that are exported, and of no others.
   The centerpiece table is read once per export.
*/
bool US_DataPubExporter::addCenterpieces( const US_DataPubCatalog::Run& run,
                                          const US_DataPubCatalog::ExpInfo& info,
                                          QString& error )
{
   QStringList serials;

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      QString serial = info.tripleCenterpieceIDs.value(
                       channel_key( run.raws[ ii ] ) );

      if ( serial.isEmpty()  ||  serial.toInt() < 1 )
         continue;

      if ( ! serials.contains( serial ) )  serials << serial;
   }

   if ( serials.isEmpty() )  return true;

   if ( ! cp_loaded )
   {
      if ( ! US_AbstractCenterpiece::read_centerpieces( catalog.db(),
                                                        cp_table ) )
      {
         error = tr( "The centerpiece table could not be read, so the"
                     " centerpieces of run %1 cannot be exported" )
                 .arg( run.runID );
         return false;
      }

      cp_loaded = true;
   }

   for ( int ii = 0; ii < serials.size(); ii++ )
   {
      int serial = serials[ ii ].toInt();
      int found  = -1;

      for ( int jj = 0; jj < cp_table.size(); jj++ )
         if ( cp_table[ jj ].serial_number == serial )  found = jj;

      if ( found < 0 )
      {
         error = tr( "Centerpiece %1 of run %2 is not in the centerpiece"
                     " table" ).arg( serial ).arg( run.runID );
         return false;
      }

      const US_AbstractCenterpiece& centerpiece = cp_table[ found ];
      QString guid = US_DataPubRecords::centerpieceGuid( centerpiece );

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
         US_AbstractCenterpiece written = centerpiece;
         written.guid = guid;

         if ( staged.isEmpty()  ||
              ! US_DataPubRecords::writeCenterpiece( written, staged ) )
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

bool US_DataPubExporter::addBuffer( US_Buffer& buffer, QString& error )
{
   if ( buffer.GUID.isEmpty() )
   {
      error = tr( "The buffer \"%1\" has no GUID and cannot be exported" )
              .arg( buffer.description );
      return false;
   }

   if ( mani.contains( US_DataPub::Buffer, buffer.GUID ) )  return true;

   if ( buffer.componentIDs.size() != buffer.concentration.size() )
   {
      error = tr( "The components of buffer \"%1\" are not readable" )
              .arg( buffer.description );
      return false;
   }

   // The buffer file names its components by ID; what the IDs stand for
   // is read here, from the database or from the local component table
   QList< US_BufferComponent > comps;

   for ( int ii = 0; ii < buffer.componentIDs.size(); ii++ )
   {
      QString compID = buffer.componentIDs[ ii ];
      bool    found  = false;

      for ( int jj = 0; jj < buffer.component.size()  &&  ! found; jj++ )
      {
         if ( buffer.component[ jj ].componentID != compID )  continue;

         comps << buffer.component[ jj ];
         found  = true;
      }

      if ( found )  continue;

      if ( catalog.isDb() )
      {
         US_BufferComponent comp;
         comp.componentID = compID;
         found            = comp.getInfoFromDB( catalog.db() );

         if ( found )  comps << comp;
      }

      else
      {
         if ( ! hd_comp_loaded )
         {
            US_BufferComponent::getAllFromHD( hd_components );
            hd_comp_loaded = true;
         }

         found = hd_components.contains( compID );

         if ( found )  comps << hd_components.value( compID );
      }

      if ( ! found )
      {
         error = tr( "Component %1 of buffer \"%2\" is not defined in the %3" )
                 .arg( compID ).arg( buffer.description )
                 .arg( catalog.isDb() ? tr( "database" )
                                      : tr( "local component table" ) );
         return false;
      }
   }

   // writeToDisk() writes one entry per component definition
   buffer.component = comps;

   US_DataPubEntity entity( US_DataPub::Buffer );
   entity.id      = buffer.bufferID;
   entity.guid    = buffer.GUID;
   entity.name    = buffer.description;
   entity.payload = US_DataPub::payloadDir( US_DataPub::Buffer ) + "/"
                    + buffer.GUID + ".xml";

   if ( ! comps.isEmpty() )
   {
      entity.attrs.insert( "componentsPayload",
                           US_DataPub::payloadDir( US_DataPub::Buffer ) + "/"
                           + buffer.GUID + ".components.xml" );
      entity.attrs.insert( "components", QString::number( comps.size() ) );
   }

   if ( stage_payloads )
   {
      QString staged = bundle.stagedPath( entity.payload );

      if ( staged.isEmpty()  ||  ! buffer.writeToDisk( staged ) )
      {
         error = tr( "The buffer %1 could not be staged" ).arg( buffer.GUID );
         return false;
      }

      if ( ! comps.isEmpty()  &&
           ! US_DataPubRecords::writeBufferComponents( comps,
                bundle.stagedPath( entity.attrs.value( "componentsPayload" ) ) ) )
      {
         error = tr( "The components of buffer %1 could not be staged" )
                 .arg( buffer.GUID );
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

   return true;
}

/* The solutions in the channels of the triples that are exported, and of
   no others: from the triple itself in a database, from the run's
   experiment XML on disk.  A triple whose solution cannot be found or read
   stops the export -- raw data cannot be imported without it.
*/
bool US_DataPubExporter::addSolutions( const US_DataPubCatalog::Run& run,
                                       const US_DataPubCatalog::ExpInfo& info,
                                       QString& error )
{
   QStringList              refs;      // solutionIDs, or GUIDs on disk
   QMap< QString, QString > rawRefs;   // rawDataGUID -> ref

   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      const US_DataPubCatalog::Raw& raw = run.raws[ ii ];
      QString ref = catalog.isDb()
                    ? raw.solutionID
                    : info.tripleSolutionGUIDs.value( channel_key( raw ) );

      if ( ref.isEmpty()  ||  ( catalog.isDb()  &&  ref.toInt() < 1 ) )
      {
         error = tr( "The raw data %1 of run %2 names no solution" )
                 .arg( raw.filename ).arg( run.runID );
         return false;
      }

      rawRefs.insert( raw.guid, ref );

      if ( ! refs.contains( ref ) )  refs << ref;
   }

   QMap< QString, QString > refGUIDs;    // ref -> solutionGUID

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

      if ( status != IUS_DB2::OK )
      {
         error = tr( "The solution %1 of run %2 could not be read: %3" )
                 .arg( refs[ ii ] ).arg( run.runID )
                 .arg( US_Solution::readErrorText( status, catalog.isDb()
                       ? catalog.db()->lastError() : QString() ) );
         return false;
      }

      if ( solution.solutionGUID.isEmpty() )
      {
         error = tr( "The solution %1 of run %2 has no GUID and cannot be"
                     " exported" ).arg( refs[ ii ] ).arg( run.runID );
         return false;
      }

      refGUIDs.insert( refs[ ii ], solution.solutionGUID );

      if ( mani.contains( US_DataPub::Solution, solution.solutionGUID ) )
         continue;

      // ---- the buffer of the solution ------------------------------------
      if ( ! addBuffer( solution.buffer, error ) )  return false;

      // ---- the analytes of the solution ----------------------------------
      QStringList anaGUIDs;

      for ( int jj = 0; jj < solution.analyteInfo.size(); jj++ )
      {
         US_Analyte analyte = solution.analyteInfo[ jj ].analyte;

         if ( analyte.analyteGUID.isEmpty() )
         {
            error = tr( "An analyte of solution \"%1\" has no GUID and"
                        " cannot be exported" ).arg( solution.solutionDesc );
            return false;
         }

         anaGUIDs << analyte.analyteGUID;

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

            if ( staged.isEmpty()  ||
                 analyte.write( false, staged ) != IUS_DB2::OK )
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
      US_DataPubEntity entity( US_DataPub::Solution );
      entity.id      = QString::number( solution.solutionID );
      entity.guid    = solution.solutionGUID;
      entity.name    = solution.solutionDesc;
      entity.payload = US_DataPub::payloadDir( US_DataPub::Solution ) + "/"
                       + solution.solutionGUID + ".xml";
      entity.setDepend( US_DataPub::Buffer, solution.buffer.GUID );
      entity.setDepend( US_DataPub::Experiment, run.guid );

      for ( int jj = 0; jj < anaGUIDs.size(); jj++ )
         entity.addDepend( US_DataPub::Analyte, anaGUIDs[ jj ] );

      if ( stage_payloads )
      {
         QString staged = bundle.stagedPath( entity.payload );

         if ( staged.isEmpty()  ||  ! solution.saveToFile( staged ) )
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

   QList< QString > rawGUIDs = rawRefs.keys();

   for ( int ii = 0; ii < rawGUIDs.size(); ii++ )
      raw_solutions.insert( rawGUIDs[ ii ],
                            refGUIDs.value( rawRefs.value( rawGUIDs[ ii ] ) ) );

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

            if ( staged.isEmpty() )
            {
               error = tr( "The edit %1 cannot be staged" ).arg( edit.filename );
               return false;
            }

            if ( catalog.isDb() )
            {
               QString why;
               int     status = US_ConvertIO::downloadEditedDataBlob(
                                catalog.db(), edit.id.toInt(), staged, why );

               if ( status != IUS_DB2::OK )
               {
                  error = tr( "The edit record %1 could not be downloaded"
                              " (status %2): %3" ).arg( edit.filename )
                          .arg( status ).arg( why );
                  return false;
               }
            }

            else if ( ! copyPayload( edit.path, staged, error ) )
               return false;

            if ( ! checkEditGuids( staged, edit.guid, raw.guid, error ) )
            {
               error = tr( "The edit %1: %2" ).arg( edit.filename )
                       .arg( error );
               return false;
            }

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

/* The models of the selected runs, narrowed to those asked for by GUID.

   A model the catalog's walk of the runs did not list -- one picked by hand
   in the model loader, say -- is looked up by its GUID and carried just the
   same; a GUID that matches no model at all fails the export.
*/
bool US_DataPubExporter::collectModels(
      const Selection& selection,
      const QList< US_DataPubCatalog::Run >& runs,
      QList< US_DataPubCatalog::Model >& models, QString& error )
{
   models.clear();

   QList< US_DataPubCatalog::Model > found = catalog.models( runs, error );

   if ( ! error.isEmpty() )  return false;

   if ( ! selection.modelsExplicit )
   {
      models = found;
      return true;
   }

   QStringList wanted = unique( selection.modelGUIDs );

   for ( int ii = 0; ii < wanted.size(); ii++ )
   {
      bool listed = false;

      for ( int jj = 0; jj < found.size()  &&  ! listed; jj++ )
      {
         if ( found[ jj ].guid != wanted[ ii ] )  continue;

         models << found[ jj ];
         listed  = true;
      }

      if ( listed )  continue;

      US_Model record;
      int      status = record.load( catalog.isDb(), wanted[ ii ],
                                     catalog.db() );

      if ( status != IUS_DB2::OK  ||  record.modelGUID != wanted[ ii ] )
      {
         error = tr( "No model with the GUID %1 was found in the %2" )
                 .arg( wanted[ ii ] )
                 .arg( catalog.isDb() ? tr( "database" )
                                      : tr( "local data directory" ) );
         return false;
      }

      US_DataPubCatalog::Model model;
      model.guid        = record.modelGUID;
      model.description = record.description;
      model.editGUID    = record.editGUID;
      models << model;

      note( tr( "Model \"%1\" is not one the selected runs list; it is"
                " exported as asked" ).arg( model.description ) );
   }

   error.clear();

   return true;
}

bool US_DataPubExporter::addModels(
      const QList< US_DataPubCatalog::Model >& models, QString& error )
{
   for ( int ii = 0; ii < models.size(); ii++ )
   {
      const US_DataPubCatalog::Model& model = models[ ii ];

      if ( model.guid.isEmpty() )
      {
         note( tr( "Warning: model \"%1\" has no GUID and is left out" )
               .arg( model.description ) );
         continue;
      }

      if ( mani.contains( US_DataPub::Model, model.guid ) )  continue;

      if ( model.editGUID.isEmpty()  ||
           ! mani.contains( US_DataPub::EditedData, model.editGUID ) )
      {
         error = tr( "Model \"%1\" needs edit %2, which is not part of"
                     " this bundle" ).arg( model.description )
                 .arg( model.editGUID.isEmpty() ? tr( "(none named)" )
                                                : model.editGUID );
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

         if ( catalog.isDb()  &&  model.id.toInt() > 0 )
            status = record.load( model.id, catalog.db() );
         else if ( ! catalog.isDb()  &&  ! model.path.isEmpty() )
            status = record.load( model.path );
         else if ( ! catalog.isDb()  &&  ! model.filename.isEmpty() )
            status = record.load( US_Settings::dataDir() + "/models/"
                                  + model.filename );
         else
            status = record.load( catalog.isDb(), model.guid, catalog.db() );

         if ( status != IUS_DB2::OK  ||  record.modelGUID != model.guid )
         {
            error = tr( "The model \"%1\" could not be read (status %2)" )
                    .arg( model.description ).arg( status );
            return false;
         }

         if ( staged.isEmpty()  ||  record.write( staged ) != IUS_DB2::OK )
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

bool US_DataPubExporter::collectNoises(
      const Selection& selection,
      const QList< US_DataPubCatalog::Model >& models,
      QList< US_DataPubCatalog::Noise >& noises, QString& error )
{
   noises.clear();

   QList< US_DataPubCatalog::Noise > found;

   if ( ! models.isEmpty() )
   {
      found = catalog.noises( models, error );

      if ( ! error.isEmpty() )  return false;
   }

   if ( ! selection.noisesExplicit )
   {
      noises = found;
      return true;
   }

   QStringList wanted = unique( selection.noiseGUIDs );
   QStringList unknown;

   for ( int ii = 0; ii < wanted.size(); ii++ )
   {
      bool listed = false;

      for ( int jj = 0; jj < found.size()  &&  ! listed; jj++ )
      {
         if ( found[ jj ].guid != wanted[ ii ] )  continue;

         noises << found[ jj ];
         listed  = true;
      }

      if ( ! listed )  unknown << wanted[ ii ];
   }

   if ( ! unknown.isEmpty() )
   {
      error = tr( "No noise record of the exported models has the GUID %1" )
              .arg( unknown.join( ", " ) );
      return false;
   }

   error.clear();

   return true;
}

bool US_DataPubExporter::addNoises(
      const QList< US_DataPubCatalog::Noise >& noises, QString& error )
{
   for ( int ii = 0; ii < noises.size(); ii++ )
   {
      const US_DataPubCatalog::Noise& noise = noises[ ii ];

      if ( noise.guid.isEmpty() )
      {
         note( tr( "Warning: noise record \"%1\" has no GUID and is left"
                   " out" ).arg( noise.description ) );
         continue;
      }

      if ( mani.contains( US_DataPub::Noise, noise.guid ) )  continue;

      if ( ! mani.contains( US_DataPub::Model, noise.modelGUID ) )
      {
         // One that was asked for by GUID is not quietly dropped
         if ( explicit_noises.contains( noise.guid ) )
         {
            error = tr( "The noise record %1 was asked for, but its model %2"
                        " is not in this bundle" ).arg( noise.guid )
                    .arg( noise.modelGUID.isEmpty() ? tr( "(none named)" )
                                                    : noise.modelGUID );
            return false;
         }

         note( tr( "Warning: noise %1 belongs to a model that is not in this"
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

      if ( ! noise.borrowedFrom.isEmpty() )
      {  // Noise of the edit, carried for a model that has none of its own
         entity.attrs.insert( "fittedToModelGUID", noise.borrowedFrom );

         note( tr( "Model %1 has no noise of its own; the %2 noise of its"
                   " edit travels with it" ).arg( noise.modelGUID )
               .arg( noise.noiseType.isEmpty() ? tr( "latest" )
                                               : noise.noiseType ) );
      }

      if ( ! noise.filename.isEmpty() )
         entity.filename = noise.filename;

      if ( stage_payloads )
      {
         QString  staged = bundle.stagedPath( entity.payload );
         US_Noise record;
         int      status;

         if ( catalog.isDb()  &&  noise.id.toInt() > 0 )
            status = record.load( noise.id, catalog.db() );
         else if ( ! catalog.isDb()  &&  ! noise.path.isEmpty() )
            status = record.load( noise.path );
         else if ( ! catalog.isDb()  &&  ! noise.filename.isEmpty() )
            status = record.load( US_Settings::dataDir() + "/noises/"
                                  + noise.filename );
         else
            status = record.load( catalog.isDb(), noise.guid, catalog.db() );

         if ( status != IUS_DB2::OK  ||  record.noiseGUID != noise.guid )
         {
            error = tr( "The noise record %1 could not be read (status %2)" )
                    .arg( noise.guid ).arg( status );
            return false;
         }

         if ( staged.isEmpty()  ||  record.write( staged ) != IUS_DB2::OK )
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

// ------------------------------------------------------------ payload checks

bool US_DataPubExporter::checkRawGuid( const QString& path,
                                       const QString& wanted, QString& error )
{
   US_DataIO::RawData data;

   if ( US_DataIO::readRawHeader( path, data ) != US_DataIO::OK )
   {
      error = tr( "the payload is not readable raw data" );
      return false;
   }

   QString found = US_Util::uuid_unparse( (unsigned char*)data.rawGUID );

   if ( found != wanted )
   {
      error = tr( "the payload carries the GUID %1, not %2" )
              .arg( found ).arg( wanted );
      return false;
   }

   error.clear();

   return true;
}

bool US_DataPubExporter::checkEditGuids( const QString& path,
                                         const QString& editGUID,
                                         const QString& rawGUID,
                                         QString& error )
{
   QFile file( path );

   if ( ! file.open( QIODevice::ReadOnly ) )
   {
      error = tr( "the payload cannot be read" );
      return false;
   }

   QXmlStreamReader xml( &file );
   QString          foundEdit;
   QString          foundRaw;
   bool             haveEdit = false;
   bool             haveRaw  = false;

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( ! xml.isStartElement() )  continue;

      QString name = xml.name().toString();

      if ( name == "editGUID"  &&  ! haveEdit )
      {
         foundEdit = xml.attributes().value( "value" ).toString();
         haveEdit  = true;
      }

      else if ( name == "rawDataGUID"  &&  ! haveRaw )
      {
         foundRaw = xml.attributes().value( "value" ).toString();
         haveRaw  = true;
      }
   }

   if ( xml.hasError() )
   {
      error = tr( "the payload is not well-formed XML: %1" )
              .arg( xml.errorString() );
      return false;
   }

   if ( foundEdit != editGUID )
   {
      error = tr( "the payload carries the edit GUID \"%1\", not %2" )
              .arg( foundEdit ).arg( editGUID );
      return false;
   }

   if ( foundRaw != rawGUID )
   {
      error = tr( "the payload belongs to raw data \"%1\", not to %2" )
              .arg( foundRaw ).arg( rawGUID );
      return false;
   }

   error.clear();

   return true;
}
