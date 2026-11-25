//! \file us_datapub_catalog.cpp
#include "us_datapub_catalog.h"
#include "us_datapub_hash.h"

#include "us_settings.h"
#include "us_time_state.h"
#include "us_util.h"

US_DataPubCatalog::ExpInfo::ExpInfo()
{
   expID         = QString( "-1" );
   projectID     = QString( "-1" );
   rotorID       = QString( "-1" );
   calibrationID = QString( "-1" );
}

bool US_DataPubCatalog::ExpInfo::readFromFile( const QString& filename )
{
   QFile file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return false;

   QXmlStreamReader xml( &file );
   QString          channelKey;

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( ! xml.isStartElement() )  continue;

      QString              ename = xml.name().toString();
      QXmlStreamAttributes attrs = xml.attributes();

      if ( ename == "experiment" )
      {
         expID    = attrs.value( "id"    ).toString();
         expGUID  = attrs.value( "guid"  ).toString();
         expType  = attrs.value( "type"  ).toString();
         runID    = attrs.value( "runID" ).toString();
      }

      else if ( ename == "project" )
      {
         projectID   = attrs.value( "id"   ).toString();
         projectGUID = attrs.value( "guid" ).toString();
         projectDesc = attrs.value( "desc" ).toString();
      }

      else if ( ename == "rotor" )
      {
         rotorID     = attrs.value( "id"     ).toString();
         rotorGUID   = attrs.value( "guid"   ).toString();
         rotorSerial = attrs.value( "serial" ).toString();
         rotorName   = attrs.value( "name"   ).toString();

         QString calID = attrs.value( "calibrationID" ).toString();

         if ( ! calID.isEmpty()  &&  calID != "0" )  calibrationID = calID;
      }

      else if ( ename == "calibration" )
      {
         QString calID = attrs.value( "id" ).toString();

         if ( ! calID.isEmpty()  &&  calID != "0" )  calibrationID = calID;

         calibrationGUID = attrs.value( "guid" ).toString();
      }

      else if ( ename == "dataset" )
      {
         channelKey = attrs.value( "cell"    ).toString() + "/"
                    + attrs.value( "channel" ).toString();
      }

      else if ( ename == "centerpiece" )
      {
         QString cpID = attrs.value( "id" ).toString();

         if ( cpID.isEmpty() )  continue;

         if ( ! centerpieceIDs.contains( cpID ) )
            centerpieceIDs << cpID;

         if ( ! channelKey.isEmpty() )
            tripleCenterpieceIDs.insert( channelKey, cpID );
      }

      else if ( ename == "solution" )
      {
         QString sguid = attrs.value( "guid" ).toString();
         QString sid   = attrs.value( "id"   ).toString();
         QString sdesc = attrs.value( "desc" ).toString();

         if ( sguid.isEmpty() )  continue;

         if ( ! channelKey.isEmpty() )
            tripleSolutionGUIDs.insert( channelKey, sguid );

         if ( solutionGUIDs.contains( sguid ) )  continue;

         solutionGUIDs << sguid;
         solutionIDs   << ( sid.isEmpty() ? QString( "-1" ) : sid );
         solutionDescs << sdesc;
      }

      else if ( ename == "label" )
      {
         xml.readNext();
         label   = xml.text().toString();
      }

      else if ( ename == "comments" )
      {
         xml.readNext();
         comments = xml.text().toString();
      }
   }

   bool failed = xml.hasError();
   file.close();

   return ! failed;
}

namespace
{
   // An UltraScan3 time stamp, whichever of the two shapes it has: the
   // database hands back "yyyy-MM-dd HH:mm:ss" (UTC: the procedures convert
   // it, timestamp2UTC) and a local file's time is the same with a " UTC"
   // suffix.  Read as UTC, never as a local time: a time of the local
   // spring-forward gap would come out shifted or invalid.
   QDateTime stamp_of( const QString& text )
   {
      qint64 seconds = 0;

      if ( ! US_DataPub::utcStampSeconds( text, seconds ) )
         return QDateTime();

      return QDateTime::fromSecsSinceEpoch( seconds, Qt::UTC );
   }

}

QList< US_DataCatalog::Noise > US_DataPubCatalog::noiseBefore(
      const QList< US_DataCatalog::Noise >& candidates,
      const QString& modelStamp )
{
   QMap< QString, US_DataCatalog::Noise > latest;
   QMap< QString, QDateTime >             latestWhen;
   QDateTime made = stamp_of( modelStamp );

   for ( int ii = 0; ii < candidates.size(); ii++ )
   {
      const US_DataCatalog::Noise& noise = candidates[ ii ];

      if ( noise.guid.isEmpty() )  continue;

      QDateTime when = stamp_of( noise.lastUpdated );

      // A record with no readable time stamp cannot be ruled out by the
      // model's; it is only used when nothing better is there.
      if ( made.isValid()  &&  when.isValid()  &&  when > made )  continue;

      QString type = noise.noiseType.isEmpty() ? QString( "ti" )
                                               : noise.noiseType.toLower();

      if ( latest.contains( type ) )
      {
         QDateTime have = latestWhen.value( type );

         if ( have.isValid()  !=  when.isValid() )
         {  // One with a time stamp beats one without
            if ( have.isValid() )  continue;
         }

         else if ( have.isValid()  &&  when != have )
         {
            if ( when < have )  continue;
         }

         else
         {  // A tie, to the second: the record written last, which in a
            // database is the higher ID; else the higher GUID -- never the
            // order the records happen to be listed in
            const US_DataCatalog::Noise& held = latest[ type ];
            int  heldID = held .id.toInt();
            int  thisID = noise.id.toInt();
            bool later  = ( heldID > 0  &&  thisID > 0 )
                          ? ( thisID > heldID )
                          : ( noise.guid > held.guid );

            if ( ! later )  continue;
         }
      }

      latest    .insert( type, noise );
      latestWhen.insert( type, when  );
   }

   return latest.values();
}

QMap< QString, QString > US_DataPubCatalog::peekAttributes(
      const QString& filename, const QString& element )
{
   QMap< QString, QString > values;
   QFile                    file( filename );

   if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )  return values;

   QXmlStreamReader xml( &file );

   while ( ! xml.atEnd() )
   {
      xml.readNext();

      if ( ! xml.isStartElement() )                     continue;
      if ( xml.name().toString() != element )           continue;

      QXmlStreamAttributes attrs = xml.attributes();

      for ( int ii = 0; ii < attrs.size(); ii++ )
         values.insert( attrs.at( ii ).name ().toString(),
                        attrs.at( ii ).value().toString() );
      break;
   }

   file.close();

   return values;
}

US_DataPubCatalog::US_DataPubCatalog()
{
   from_db = false;
   dbase   = nullptr;
   inv_id  = US_Settings::us_inv_ID();
   cat     = new US_DataCatalog();
   listed  = false;
   cancel_flag = nullptr;
   model_file_scans = 0;
}

US_DataPubCatalog::~US_DataPubCatalog()
{
   delete cat;

   cat   = nullptr;
   dbase = nullptr;
}

bool US_DataPubCatalog::open( bool fromDb, const QString& dbPassword,
                              QString& error )
{
   from_db = fromDb;
   listed  = false;
   listed_project.clear();
   model_files_dir.clear();

   // A wrong master password would otherwise be reported as the server's
   // refusal of a login without a password
   if ( fromDb )
   {
      QString problem = US_DataPub::masterPasswordProblem( dbPassword );

      if ( ! problem.isEmpty() )
      {
         dbase = nullptr;
         error = problem;
         return false;
      }
   }

   if ( ! cat->open( fromDb ? US_DataCatalog::Db : US_DataCatalog::Disk,
                     dbPassword, error ) )
   {
      dbase = nullptr;
      return false;
   }

   dbase  = dynamic_cast< US_DB2* >( cat->db() );   // open() made a US_DB2
   inv_id = cat->investigatorID();

   error.clear();

   return true;
}

/* The investigator can be switched from a dialog while this catalog is
   open -- the run selection dialog offers one -- and the database holds one
   person's records at a time.  What was listed under the person selected
   before then describes records the user is no longer looking at, so the
   run they just picked would be reported as missing.  Forgetting the
   listing makes the next query read the person who is selected now.
*/
void US_DataPubCatalog::syncInvestigator( void )
{
   if ( ! cat->refreshInvestigator() )  return;

   inv_id = cat->investigatorID();
   listed = false;
   listed_project.clear();
}

// Read the first layer of the shared catalog, once per project filter
bool US_DataPubCatalog::ensureListed( const QString& projectGUID,
                                      QString& error )
{
   syncInvestigator();

   if ( listed  &&  listed_project == projectGUID )
   {
      error.clear();
      return true;
   }

   if ( ! cat->loadRuns( projectGUID, error ) )  return false;

   listed         = true;
   listed_project = projectGUID;

   return true;
}

void US_DataPubCatalog::attachDb( IUS_DB2* db )
{
   from_db = true;
   dbase   = db;
   listed  = false;
   listed_project.clear();

   // The chain reads through the same connection when it can; a test
   // double that says it is not connected leaves the chain without one
   QString error;
   cat->attach( db, error );
}

bool US_DataPubCatalog::isDb( void ) const
{
   return from_db;
}

void US_DataPubCatalog::setBulkQueries( bool on )
{
   cat->setBulkQueries( on );
}

void US_DataPubCatalog::setCancelFlag( const QAtomicInt* flag )
{
   cancel_flag = flag;
   cat->setCancelFlag( flag );
}

bool US_DataPubCatalog::cancelledError( QString& error ) const
{
   if ( cancel_flag == nullptr  ||  cancel_flag->loadAcquire() == 0 )
      return false;

   error = cancelledText();
   return true;
}

void US_DataPubCatalog::setFileNameCase( Qt::CaseSensitivity sensitivity )
{
   cat->setFileNameCase( sensitivity );
   listed = false;
   listed_project.clear();
}

QString US_DataPubCatalog::cancelledText( void ) const
{
   return QObject::tr( "Reading the %1 was cancelled" )
          .arg( from_db ? QObject::tr( "database" )
                        : QObject::tr( "local data directory" ) );
}

void US_DataPubCatalog::setMessageHandler(
      const std::function< void( const QString& ) >& handler )
{
   QObject::disconnect( message_link );
   message_link = QMetaObject::Connection();

   if ( ! handler )  return;

   message_link = QObject::connect( cat, &US_DataCatalog::message, cat,
                                    [ handler ]( const QString& text )
                                    { handler( text ); } );
}

IUS_DB2* US_DataPubCatalog::db( void )
{
   return dbase;
}

int US_DataPubCatalog::modelFileScans( void ) const
{
   return model_file_scans;
}

int US_DataPubCatalog::investigatorID( void ) const
{
   return inv_id;
}

// ---------------------------------------------------------------- projects

QList< US_DataPubCatalog::Project > US_DataPubCatalog::projects(
      QString& error )
{
   syncInvestigator();

   return from_db ? projectsDb( error ) : projectsDisk( error );
}

QList< US_DataPubCatalog::Project > US_DataPubCatalog::projectsDb(
      QString& error )
{
   QList< Project > list;

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return list;
   }

   QStringList query;
   query << "get_project_desc" << QString::number( inv_id );
   dbase->query( query );

   if ( dbase->lastErrno() != IUS_DB2::OK  &&
        dbase->lastErrno() != IUS_DB2::NOROWS )
   {
      error = QObject::tr( "The projects could not be listed: %1" )
              .arg( dbase->lastError() );
      return list;
   }

   QStringList ids;

   while ( dbase->next() )
      ids << dbase->value( 0 ).toString();

   for ( int ii = 0; ii < ids.size(); ii++ )
   {
      Project project;
      project.id = ids[ ii ];

      query.clear();
      query << "get_project_info" << project.id;
      dbase->query( query );

      if ( dbase->lastErrno() != IUS_DB2::OK  ||  ! dbase->next() )
      {
         error = QObject::tr( "The project %1 could not be read: %2" )
                 .arg( project.id ).arg( dbase->lastError() );
         list.clear();
         return list;
      }

      project.guid        = dbase->value(  1 ).toString();
      project.description = dbase->value( 10 ).toString();

      list << project;
   }

   error.clear();

   return list;
}

QList< US_DataPubCatalog::Project > US_DataPubCatalog::projectsDisk(
      QString& error )
{
   QList< Project > list;
   QString          path = US_Settings::dataDir() + "/projects";
   QStringList      files = QDir( path ).entryList(
                            QStringList( "P???????.xml" ),
                            QDir::Files, QDir::Name );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QString                  fpath = path + "/" + files[ ii ];
      QMap< QString, QString > attrs = peekAttributes( fpath, "project" );

      if ( attrs.isEmpty() )  continue;

      Project project;
      project.id       = attrs.value( "id", "-1" );
      project.guid     = attrs.value( "guid" );
      project.filename = files[ ii ];

      QFile file( fpath );

      if ( file.open( QIODevice::ReadOnly | QIODevice::Text ) )
      {
         QXmlStreamReader xml( &file );

         while ( ! xml.atEnd() )
         {
            xml.readNext();

            if ( xml.isStartElement()  &&
                 xml.name().toString() == "description" )
            {
               project.description = xml.readElementText();
               break;
            }
         }

         file.close();
      }

      list << project;
   }

   error.clear();

   return list;
}

// -------------------------------------------------------------------- runs
//
// The record chain comes from the shared catalog.  The first layer lists
// the experiments; the second reads one experiment's triples, edits, models
// and noise records, and an export harvests what it needs from that.

QList< US_DataPubCatalog::Run > US_DataPubCatalog::runs(
      const QString& projectGUID, QString& error )
{
   QList< Run > list;

   if ( ! ensureListed( projectGUID, error ) )  return list;

   return cat->runs();
}

QList< int > US_DataPubCatalog::indexesOfRun( const QString& runID ) const
{
   QList< int > found;

   // Every run the source takes for this run ID, by the rule of
   // US_DataCatalog::indexOfRun(): the database's runID column does not
   // tell "RunA" from "runa", so both are experiments of the one run ID
   // (and more than one is refused by runByID()); a local store compares
   // as its file system does, and can hold the run in more than one
   // directory
   Qt::CaseSensitivity cs = from_db ? Qt::CaseInsensitive
                                    : cat->fileNameCase();

   for ( int ii = 0; ii < cat->runCount(); ii++ )
      if ( cat->run( ii ).runID.compare( runID, cs ) == 0 )
         found << ii;

   return found;
}

QList< US_DataPubCatalog::Run > US_DataPubCatalog::experimentsOfRun(
      const QString& runID, QString& error )
{
   QList< Run > list;

   if ( ! ensureListed( listed ? listed_project : QString(), error ) )
      return list;

   QList< int > indexes = indexesOfRun( runID );

   if ( indexes.isEmpty()  &&  ! listed_project.isEmpty() )
   {  // Not in the project that was listed, as runByID() looks
      if ( ! ensureListed( QString(), error ) )  return list;

      indexes = indexesOfRun( runID );
   }

   for ( int ii = 0; ii < indexes.size(); ii++ )
      list << cat->run( indexes[ ii ] );

   std::stable_sort( list.begin(), list.end(),
                     []( const Run& aa, const Run& bb )
                     { return aa.id.toInt() < bb.id.toInt(); } );

   error.clear();

   return list;
}

int US_DataPubCatalog::indexOfListedRun( const Run& run ) const
{
   QList< int > found = indexesOfRun( run.runID );

   // A database run is its experiment, a disk run its directory
   for ( int ii = 0; ii < found.size(); ii++ )
   {
      const Run& listedRun = cat->run( found[ ii ] );

      if ( from_db ? ( run.id.toInt() < 1  ||
                       listedRun.id.toInt() == run.id.toInt() )
                   : listedRun.dirPath == run.dirPath )
         return found[ ii ];
   }

   return -1;
}

bool US_DataPubCatalog::runByID( const QString& runID, Run& found,
                                 QString& error, bool* unusable,
                                 QStringList* copies,
                                 const QString& experimentID )
{
   if ( unusable != nullptr )  *unusable = false;
   if ( copies   != nullptr )  copies->clear();

   if ( ! ensureListed( listed ? listed_project : QString(), error ) )
      return false;

   QList< int > indexes = indexesOfRun( runID );

   if ( indexes.isEmpty()  &&  ! listed_project.isEmpty() )
   {  // Not in the project that was listed; look through all of them
      if ( ! ensureListed( QString(), error ) )  return false;

      indexes = indexesOfRun( runID );
   }

   if ( indexes.isEmpty() )
   {
      error = QObject::tr( "No run \"%1\" was found in the %2" ).arg( runID )
              .arg( from_db ? QObject::tr( "database" )
                            : QObject::tr( "local results directory" ) );
      return false;
   }

   // A database can hold several experiments of one investigator under
   // one run ID.  Only an experiment ID tells them apart; the run ID alone
   // names none of them.
   if ( from_db  &&  ! experimentID.isEmpty() )
   {
      QList< int > byID;
      QStringList  ids;

      auto pick = [ & ]( void )
      {
         byID.clear();
         ids .clear();

         for ( int ii = 0; ii < indexes.size(); ii++ )
         {
            const Run& run = cat->run( indexes[ ii ] );

            ids << run.id;

            if ( run.id.toInt() > 0  &&
                 run.id.toInt() == experimentID.toInt() )
               byID << indexes[ ii ];
         }
      };

      pick();

      if ( byID.isEmpty()  &&  ! listed_project.isEmpty() )
      {  // The experiment named is not in the project listed: the caller
         // says what is wrong with one of another project
         if ( ! ensureListed( QString(), error ) )  return false;

         indexes = indexesOfRun( runID );
         pick();
      }

      if ( byID.isEmpty() )
      {
         error = QObject::tr( "Run %1 has no experiment with the ID %2 in the"
                              " database (its experiment ID: %3)" )
                 .arg( runID ).arg( experimentID ).arg( ids.join( ", " ) );
         return false;
      }

      indexes = byID;
   }

   else if ( from_db  &&  indexes.size() > 1 )
   {
      QList< int > ids;

      for ( int ii = 0; ii < indexes.size(); ii++ )
         ids << cat->run( indexes[ ii ] ).id.toInt();

      std::sort( ids.begin(), ids.end() );

      QStringList idText;

      for ( int ii = 0; ii < ids.size(); ii++ )
         idText << QString::number( ids[ ii ] );

      if ( unusable != nullptr )  *unusable = true;

      error = QObject::tr( "Run %1 is the run ID of %2 experiments in the"
                           " database, with the experiment IDs %3; which of"
                           " them is meant cannot be told from the run ID."
                           "  Select one by its experiment ID"
                           " (--experiment-id <ID> on the command line)" )
              .arg( runID ).arg( ids.size() ).arg( idText.join( ", " ) );
      return false;
   }

   // The one usable run of the run ID, whichever position it has: a stray
   // copy of its directory, listed before it or not, is not taken for it.
   // The catalog marks a copy as such only when the run's own directory --
   // the one named after it -- is there.
   QList< int > usable;
   int          own = -1;          // the run's own directory, beside copies
   QStringList  copyDirs;

   for ( int ii = 0; ii < indexes.size(); ii++ )
   {
      const Run& run = cat->run( indexes[ ii ] );

      if ( run.usable )
         usable << indexes[ ii ];

      if ( ! run.copyOf.isEmpty() )
         copyDirs << QDir::toNativeSeparators( run.dirPath );
      else if ( own < 0 )
         own = indexes[ ii ];
   }

   copyDirs.sort();

   if ( usable.size() == 1 )
   {
      found = cat->run( usable[ 0 ] );

      if ( copies != nullptr )  *copies = copyDirs;

      error.clear();
      return true;
   }

   if ( unusable != nullptr )  *unusable = true;

   if ( own >= 0  &&  ! copyDirs.isEmpty() )
   {  // The run's own directory is there, and is what is wrong
      const Run& run = cat->run( own );

      error = QObject::tr( "Run %1 cannot be used as it stands: %2" )
              .arg( runID )
              .arg( run.problem.isEmpty() ? QObject::tr( "the catalog marks"
                                                         " it as not usable" )
                                          : run.problem );
      return false;
   }

   if ( indexes.size() > 1 )
   {
      QStringList dirs;

      for ( int ii = 0; ii < indexes.size(); ii++ )
         dirs << QDir::toNativeSeparators( cat->run( indexes[ ii ] ).dirPath );

      dirs.sort();

      error = QObject::tr( "Run %1 is in %2 directories: %3.  Which of them"
                           " is the run cannot be told, since none is named"
                           " after it; keep only the directory of the run in"
                           " the results directory, named after the run, and"
                           " move the copies elsewhere" )
              .arg( runID ).arg( dirs.size() ).arg( dirs.join( ", " ) );
      return false;
   }

   const Run& run = cat->run( indexes[ 0 ] );

   error = QObject::tr( "Run %1 cannot be used as it stands: %2" )
           .arg( runID )
           .arg( run.problem.isEmpty() ? QObject::tr( "the catalog marks it"
                                                      " as not usable" )
                                       : run.problem );
   return false;
}

QString US_DataPubCatalog::expFilePath( const Run& run )
{
   if ( run.dirPath.isEmpty() )  return QString();

   return run.dirPath + "/" + run.runID + "." + run.runType + ".xml";
}

// ------------------------------------------------------------- run details

bool US_DataPubCatalog::loadRunDetails( Run& run, QString& error )
{
   // A database read cannot be stopped part way; it is not started
   if ( cancelledError( error ) )  return false;

   if ( ! ensureListed( listed ? listed_project : QString(), error ) )
      return false;

   // The run in its own directory, not another one of its run ID
   int index = indexOfListedRun( run );

   if ( index < 0  &&  ! listed_project.isEmpty() )
   {
      if ( ! ensureListed( QString(), error ) )  return false;

      index = indexOfListedRun( run );
   }

   if ( index < 0 )
   {
      error = QObject::tr( "No run \"%1\" was found in the %2" )
              .arg( run.runID )
              .arg( from_db ? QObject::tr( "database" )
                            : QObject::tr( "local results directory" ) );
      return false;
   }

   if ( ! cat->loadRunDetail( index, error ) )  return false;

   run = cat->run( index );

   error.clear();

   return true;
}

// ------------------------------------------------------------------ models

QList< US_DataPubCatalog::Model > US_DataPubCatalog::models(
      const QList< Run >& runList, QString& error )
{
   QList< Model > list;
   QStringList    seen;

   for ( int ii = 0; ii < runList.size(); ii++ )
   {
      const Run& run = runList[ ii ];

      for ( int jj = 0; jj < run.raws.size(); jj++ )
      {
         const Raw& raw = run.raws[ jj ];

         for ( int kk = 0; kk < raw.edits.size(); kk++ )
         {
            const Edit& edit = raw.edits[ kk ];

            for ( int mm = 0; mm < edit.models.size(); mm++ )
            {
               const Model& model = edit.models[ mm ];

               if ( model.guid.isEmpty() )       continue;
               if ( seen.contains( model.guid ) )  continue;

               seen << model.guid;
               list << model;
            }
         }
      }
   }

   error.clear();

   return list;
}

bool US_DataPubCatalog::completeModel( Model& model, QString& error )
{
   error.clear();

   if ( model.guid.isEmpty() )  return true;

   if ( ! from_db )
   {
      // The model files are read once, for every model looked up after:
      // GUID -> the first file (by name) that holds it
      QString mdir = US_Settings::dataDir() + "/models";

      if ( model_files_dir != mdir )
      {
         QStringList files = QDir( mdir ).entryList(
                             QStringList( "M???????.xml" ), QDir::Files,
                             QDir::Name );

         model_files.clear();
         model_files_dir = mdir;
         model_file_scans++;

         for ( int ii = 0; ii < files.size(); ii++ )
         {
            QString guid = peekAttributes( mdir + "/" + files[ ii ], "model" )
                           .value( "modelGUID" );

            if ( ! guid.isEmpty()  &&  ! model_files.contains( guid ) )
               model_files.insert( guid, files[ ii ] );
         }
      }

      QString file = model_files.value( model.guid );

      if ( file.isEmpty() )  return true;

      QString path      = mdir + "/" + file;
      model.filename    = file;
      model.path        = path;
      model.lastUpdated = QLocale::c().toString(
            QFileInfo( path ).lastModified().toUTC(),
            "yyyy-MM-dd hh:mm:ss" ) + " UTC";

      return true;
   }

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return false;
   }

   auto failed = [ this, &error, &model ]( void )
   {
      int status = dbase->lastErrno();

      if ( status == IUS_DB2::OK  ||  status == IUS_DB2::NOROWS )
         return false;

      error = QObject::tr( "The model %1 could not be looked up: %2" )
              .arg( model.guid ).arg( dbase->lastError() );
      return true;
   };

   QStringList query;
   query << "get_modelID" << model.guid;
   dbase->query( query );

   if ( failed() )  return false;

   int id = dbase->next() ? dbase->value( 0 ).toInt() : 0;

   if ( id < 1 )  return true;

   model.id = QString::number( id );

   query.clear();
   query << "get_model_info" << model.id;
   dbase->query( query );

   if ( failed() )  return false;

   // get_model_info: modelGUID, description, xml, variance, meniscus,
   // personID, lastUpdated (UTC), ...
   if ( dbase->next()  &&  dbase->value( 0 ).toString() == model.guid )
      model.lastUpdated = US_DataCatalog::utcText( dbase->value( 6 ) );

   // The edit it was fitted to, by ID: the noise of that edit is what a
   // model without noise of its own borrows (noises(), noisesOfEdit())
   if ( model.editID.isEmpty()  &&  ! model.editGUID.isEmpty() )
   {
      query.clear();
      query << "get_editID" << model.editGUID;
      dbase->query( query );

      if ( failed() )  return false;

      int editID = dbase->next() ? dbase->value( 0 ).toInt() : 0;

      if ( editID > 0 )
         model.editID = QString::number( editID );
   }

   return true;
}

// ------------------------------------------------------------------ noises
QList< US_DataPubCatalog::Noise > US_DataPubCatalog::noises(
      const QList< Model >& modelList, QString& error,
      QMultiHash< QString, QString >* borrowers )
{
   QList< Noise > list;
   QStringList    seen;

   error.clear();

   if ( borrowers != nullptr )  borrowers->clear();

   for ( int ii = 0; ii < modelList.size(); ii++ )
   {
      const Model& model = modelList[ ii ];

      // Each lookup below can be a database query of its own
      if ( cancelledError( error ) )  return list;

      // A model that came out of the chain carries its noise already; one
      // the user picked by hand, whose edit was not among those read, has
      // to be looked up in the source.
      QList< US_DataCatalog::Noise > found = model.noises;

      if ( found.isEmpty() )
      {
         QString message;
         found = cat->noisesOfModel( model.guid, message );

         if ( ! message.isEmpty() )  error = message;
      }

      bool borrowed = false;

      if ( found.isEmpty() )
      {  // Nothing was fitted to this model, so the noise of its edit that
         // was there when the model was made travels with it instead
         QString message;
         QList< US_DataCatalog::Noise > siblings =
            cat->noisesOfEdit( model.editGUID, model.editID, message );

         if ( ! message.isEmpty() )  error = message;

         found    = noiseBefore( siblings, model.lastUpdated );
         borrowed = ! found.isEmpty();
      }

      for ( int jj = 0; jj < found.size(); jj++ )
      {
         Noise noise( found[ jj ] );

         if ( noise.guid.isEmpty() )         continue;

         // Every model that borrows it, also one that is not the first
         // to list it
         if ( borrowed  &&  borrowers != nullptr  &&
              ! borrowers->contains( noise.guid, model.guid ) )
            borrowers->insert( noise.guid, model.guid );

         if ( seen.contains( noise.guid ) )  continue;

         if ( noise.editGUID.isEmpty() )
            noise.editGUID = model.editGUID;

         if ( borrowed )
         {  // It belongs to another model of the same edit.  The bundle
            // carries it for this one, and says where it came from.
            noise.borrowedFrom = noise.modelGUID;
            noise.modelGUID    = model.guid;
         }

         seen << noise.guid;
         list << noise;
      }
   }

   return list;
}

// -------------------------------------------------------- experiment info

bool US_DataPubCatalog::expInfo( const Run& run, ExpInfo& info,
                                 QString& error, bool* runFault )
{
   syncInvestigator();

   if ( runFault != nullptr )  *runFault = false;

   if ( ! from_db )
   {
      QString fpath = expFilePath( run );

      // The run's own files are wrong; the store as such is fine
      if ( runFault != nullptr )  *runFault = true;

      if ( fpath.isEmpty()  ||  ! QFile( fpath ).exists() )
      {
         error = QObject::tr( "No experiment XML file was found for run %1" )
                 .arg( run.runID );
         return false;
      }

      if ( ! info.readFromFile( fpath ) )
      {
         error = QObject::tr( "The experiment XML file of run %1 could"
                              " not be read: %2" ).arg( run.runID )
                 .arg( QDir::toNativeSeparators( fpath ) );
         return false;
      }

      if ( runFault != nullptr )  *runFault = false;

      error.clear();
      return true;
   }

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return false;
   }

   // A query that failed is an error; one that found nothing is for the
   // caller to judge
   auto failed = [ this, &error, &run ]( const QString& what )
   {
      int status = dbase->lastErrno();

      if ( status == IUS_DB2::OK  ||  status == IUS_DB2::NOROWS )
         return false;

      error = QObject::tr( "The %1 of run %2 could not be read: %3" )
              .arg( what ).arg( run.runID ).arg( dbase->lastError() );
      return true;
   };

   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "get_experiment_info_by_runID" << run.runID << invID;
   dbase->query( query );

   if ( failed( QObject::tr( "experiment" ) ) )  return false;

   // The row of the run's own experiment: an investigator can have more
   // than one experiment of a run ID, and the procedure gives every one
   bool byID  = run.id.toInt() > 0;
   bool found = false;

   while ( dbase->next() )
   {
      if ( byID  &&  dbase->value( 1 ).toInt() != run.id.toInt() )  continue;

      found = true;
      break;
   }

   if ( ! found )
   {
      error = byID ? QObject::tr( "Run %1 (experiment ID %2) was not found in"
                                  " the database" ).arg( run.runID )
                     .arg( run.id )
                   : QObject::tr( "Run %1 was not found in the database" )
                     .arg( run.runID );

      if ( runFault != nullptr )  *runFault = true;

      return false;
   }

   info.projectID     = dbase->value(  0 ).toString();
   info.expID         = dbase->value(  1 ).toString();
   info.expGUID       = dbase->value(  2 ).toString();
   info.rotorID       = dbase->value(  6 ).toString();
   info.calibrationID = dbase->value(  7 ).toString();
   info.expType       = dbase->value(  8 ).toString();
   info.label         = dbase->value( 10 ).toString();
   info.comments      = dbase->value( 11 ).toString();
   info.runID         = run.runID;

   // NULL, not empty: new_experiment stores '' for both, so an import keeps
   // the NULL only when the bundle says so (the "nullFields" attribute)
   // (A column the row does not have is not NULL.)
   info.nullFields.clear();

   auto isNull = [ this ]( int column )
   {
      QVariant value = dbase->value( column );
      return value.isValid()  &&  value.toString().isNull();
   };

   if ( isNull( 12 ) )  info.nullFields << "centrifugeProtocol";
   if ( isNull( 16 ) )  info.nullFields << "RIProfile";

   // A NULL column comes back as "", which a server in strict mode refuses
   // as an integer argument ("Incorrect integer value"): a record that
   // names no project or rotor is not looked up at all
   if ( info.projectID.toInt() > 0 )
   {
      query.clear();
      query << "get_project_info" << info.projectID;
      dbase->query( query );

      if ( failed( QObject::tr( "project" ) ) )  return false;

      if ( dbase->next() )
      {
         info.projectGUID = dbase->value(  1 ).toString();
         info.projectDesc = dbase->value( 10 ).toString();
      }
   }

   else
      info.projectID.clear();

   if ( info.rotorID.toInt() > 0 )
   {
      query.clear();
      query << "get_rotor_info" << info.rotorID;
      dbase->query( query );

      if ( failed( QObject::tr( "rotor" ) ) )  return false;

      if ( dbase->next() )
      {
         info.rotorGUID   = dbase->value( 0 ).toString();
         info.rotorName   = dbase->value( 1 ).toString();
         info.rotorSerial = dbase->value( 2 ).toString();
      }
   }

   else
      info.rotorID.clear();

   // Older records do not name a calibration.  The rotor's first one is
   // not assumed in its place: a rotor is recalibrated over the years, and
   // a bundle should not state which calibration a run was made with when
   // the record does not say.  The run then travels without one.
   if ( info.calibrationID.toInt() < 1 )
      info.calibrationID.clear();

   const QString letters = US_Util::channel_letters();

   query.clear();

   if ( info.expID.toInt() > 0 )
   {
      query << "all_cell_experiments" << info.expID;
      dbase->query( query );

      if ( failed( QObject::tr( "cell assignments" ) ) )  return false;
   }

   while ( ! query.isEmpty()  &&  dbase->next() )
   {
      QString cell  = dbase->value( 2 ).toString();
      int     chanx = qMax( 0, dbase->value( 3 ).toInt() );
      chanx         = qMin( chanx, letters.size() - 1 );
      QString cpID  = dbase->value( 4 ).toString();

      if ( cpID.isEmpty() )  continue;

      if ( ! info.centerpieceIDs.contains( cpID ) )
         info.centerpieceIDs << cpID;

      info.tripleCenterpieceIDs.insert( cell + "/" + QString( letters[ chanx ] ),
                                        cpID );
   }

   // The solutions are those of the triples the run brings, not every
   // solution ever associated with the experiment
   for ( int ii = 0; ii < run.raws.size(); ii++ )
   {
      QString solID = run.raws[ ii ].solutionID;

      if ( solID.toInt() > 0  &&  ! info.solutionIDs.contains( solID ) )
         info.solutionIDs << solID;
   }

   error.clear();

   return true;
}

// -------------------------------------------------------------- time state

US_DataPubCatalog::TimeStateStatus US_DataPubCatalog::timeState(
      const Run& run, const QString& workDir, bool fetch,
      QString& tmstPath, QString& xdefPath, QString& tmstID, QString& error )
{
   QString base = run.runID + ".time_state.tmst";

   tmstPath.clear();
   xdefPath.clear();
   tmstID = QString( "-1" );
   error.clear();

   if ( ! from_db )
   {
      QString dirPath = run.dirPath.isEmpty()
                        ? US_Settings::resultDir() + "/" + run.runID
                        : run.dirPath;
      tmstPath = dirPath + "/" + base;
      xdefPath = US_DataPub::timeStateDefs( tmstPath );

      if ( ! QFile::exists( tmstPath ) )  return TimeStateNone;

      if ( ! QFile::exists( xdefPath ) )
      {
         error = QObject::tr( "The time state of run %1 has no field"
                              " definitions file (%2), so it cannot be read;"
                              " leave the time state out to export the run"
                              " without it" )
                 .arg( run.runID ).arg( QFileInfo( xdefPath ).fileName() );
         return TimeStateBroken;
      }

      return TimeStateFound;
   }

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return TimeStateFailed;
   }

   int       tmstDbID = 0;
   int       expID    = run.id.toInt();
   QString   fname    = base;
   QString   xdefs;
   QString   cksum;
   QDateTime updated;

   int status = US_TimeState::dbExamine( dbase, &tmstDbID, &expID, &fname,
                                         &xdefs, &cksum, &updated );

   if ( status == IUS_DB2::NOROWS )  return TimeStateNone;

   if ( status != IUS_DB2::OK  ||  tmstDbID <= 0 )
   {
      error = QObject::tr( "The time state of run %1 could not be looked up"
                           " (status %2): %3" )
              .arg( run.runID ).arg( status ).arg( dbase->lastError() );
      return TimeStateFailed;
   }

   tmstID = QString::number( tmstDbID );

   if ( xdefs.trimmed().isEmpty() )
   {
      error = QObject::tr( "The time state of run %1 has no field"
                           " definitions in the database, so it cannot be"
                           " read; leave the time state out to export the"
                           " run without it" ).arg( run.runID );
      return TimeStateBroken;
   }

   // The record keeps a file name of its own, but a bundle names a time
   // state after its run, as the disk store does: an import refuses a run's
   // file whose name does not start with "<runID>.", and the record's name
   // may be a path, another run's name or anything else
   tmstPath = workDir + "/" + base;
   xdefPath = US_DataPub::timeStateDefs( tmstPath );

   if ( ! fetch )  return TimeStateFound;

   if ( ! QDir().mkpath( workDir ) )
   {
      error = QObject::tr( "Cannot create the work directory %1" )
              .arg( workDir );
      return TimeStateFailed;
   }

   status = US_TimeState::dbDownload( dbase, tmstDbID, tmstPath );

   if ( status != IUS_DB2::OK  ||  ! QFile::exists( tmstPath ) )
   {
      error = QObject::tr( "The time state of run %1 could not be downloaded"
                           " (status %2): %3" )
              .arg( run.runID ).arg( status ).arg( dbase->lastError() );
      return TimeStateFailed;
   }

   QSaveFile  xfile( xdefPath );
   QByteArray bytes = xdefs.toUtf8();

   if ( ! xfile.open( QIODevice::WriteOnly )  ||
        xfile.write( bytes ) != bytes.size()  ||  ! xfile.commit() )
   {
      error = QObject::tr( "The field definitions of the time state of run %1"
                           " could not be written to %2" )
              .arg( run.runID ).arg( xdefPath );
      return TimeStateFailed;
   }

   return TimeStateFound;
}
