//! \file us_datapub_catalog.cpp
#include "us_datapub_catalog.h"
#include "us_datapub_hash.h"

#include "us_settings.h"
#include "us_time_state.h"

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
   // database hands back "yyyy-MM-dd HH:mm:ss" and a local file's time is
   // the same with a " UTC" suffix.
   QDateTime stamp_of( const QString& text )
   {
      QString trimmed = text.trimmed();

      if ( trimmed.endsWith( " UTC" ) )
         trimmed.chop( 4 );

      trimmed.replace( "T", " " );

      QDateTime when = QDateTime::fromString( trimmed, "yyyy-MM-dd HH:mm:ss" );

      if ( ! when.isValid() )
         when = QDateTime::fromString( trimmed, Qt::ISODate );

      return when;
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

         if ( have.isValid()  &&  when.isValid()  &&  when <= have )
            continue;

         if ( have.isValid()  &&  ! when.isValid() )  continue;
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

   if ( ! cat->open( fromDb ? US_DataCatalog::Db : US_DataCatalog::Disk,
                     dbPassword, error ) )
   {
      dbase = nullptr;
      return false;
   }

   dbase  = cat->db();
   inv_id = cat->investigatorID();

   error.clear();

   return true;
}

// Read the first layer of the shared catalog, once per project filter
bool US_DataPubCatalog::ensureListed( const QString& projectGUID,
                                      QString& error )
{
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

bool US_DataPubCatalog::isDb( void ) const
{
   return from_db;
}

US_DB2* US_DataPubCatalog::db( void )
{
   return dbase;
}

int US_DataPubCatalog::investigatorID( void ) const
{
   return inv_id;
}

// ---------------------------------------------------------------- projects

QList< US_DataPubCatalog::Project > US_DataPubCatalog::projects(
      QString& error )
{
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

   if ( dbase->lastErrno() != US_DB2::OK  &&
        dbase->lastErrno() != US_DB2::NOROWS )
   {
      error = dbase->lastError();
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

      if ( dbase->next() )
      {
         project.guid        = dbase->value(  1 ).toString();
         project.description = dbase->value( 10 ).toString();
      }

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

bool US_DataPubCatalog::runByID( const QString& runID, Run& found,
                                 QString& error )
{
   if ( ! ensureListed( listed ? listed_project : QString(), error ) )
      return false;

   int index = cat->indexOfRun( runID );

   if ( index < 0  &&  ! listed_project.isEmpty() )
   {  // Not in the project that was listed; look through all of them
      if ( ! ensureListed( QString(), error ) )  return false;

      index = cat->indexOfRun( runID );
   }

   if ( index < 0 )
   {
      error = QObject::tr( "No run \"%1\" was found in the %2" ).arg( runID )
              .arg( from_db ? QObject::tr( "database" )
                            : QObject::tr( "local results directory" ) );
      return false;
   }

   found = cat->run( index );
   error.clear();

   return true;
}

QString US_DataPubCatalog::expFilePath( const Run& run )
{
   if ( run.dirPath.isEmpty() )  return QString();

   return run.dirPath + "/" + run.runID + "." + run.runType + ".xml";
}

// ------------------------------------------------------------- run details

bool US_DataPubCatalog::loadRunDetails( Run& run, QString& error )
{
   if ( ! ensureListed( listed ? listed_project : QString(), error ) )
      return false;

   int index = cat->indexOfRun( run.runID );

   if ( index < 0  &&  ! listed_project.isEmpty() )
   {
      if ( ! ensureListed( QString(), error ) )  return false;

      index = cat->indexOfRun( run.runID );
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

   if ( run.raws.isEmpty() )
   {
      error = QObject::tr( "No raw data was found for run \"%1\"" )
              .arg( run.runID );
      return false;
   }

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

// ------------------------------------------------------------------ noises

QList< US_DataPubCatalog::Noise > US_DataPubCatalog::noises(
      const QList< Model >& modelList, QString& error )
{
   QList< Noise > list;
   QStringList    seen;

   error.clear();

   for ( int ii = 0; ii < modelList.size(); ii++ )
   {
      const Model& model = modelList[ ii ];

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
                                 QString& error )
{
   if ( ! from_db )
   {
      QString fpath = expFilePath( run );

      if ( fpath.isEmpty()  ||  ! QFile( fpath ).exists() )
      {
         error = QObject::tr( "No experiment XML file was found for run %1" )
                 .arg( run.runID );
         return false;
      }

      if ( ! info.readFromFile( fpath ) )
      {
         error = QObject::tr( "The experiment XML file of run %1 could"
                              " not be read" ).arg( run.runID );
         return false;
      }

      error.clear();
      return true;
   }

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return false;
   }

   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "get_experiment_info_by_runID" << run.runID << invID;
   dbase->query( query );

   if ( ! dbase->next() )
   {
      error = QObject::tr( "Run %1 was not found in the database" )
              .arg( run.runID );
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

   query.clear();
   query << "get_project_info" << info.projectID;
   dbase->query( query );

   if ( dbase->next() )
   {
      info.projectGUID = dbase->value(  1 ).toString();
      info.projectDesc = dbase->value( 10 ).toString();
   }

   query.clear();
   query << "get_rotor_info" << info.rotorID;
   dbase->query( query );

   if ( dbase->next() )
   {
      info.rotorGUID   = dbase->value( 0 ).toString();
      info.rotorName   = dbase->value( 1 ).toString();
      info.rotorSerial = dbase->value( 2 ).toString();
   }

   if ( info.calibrationID == "0"  ||  info.calibrationID.isEmpty() )
   {  // Older records do not name a calibration; take the rotor's first
      query.clear();
      query << "get_rotor_calibration_profiles" << info.rotorID;
      dbase->query( query );

      if ( dbase->next() )
         info.calibrationID = dbase->value( 0 ).toString();
   }

   query.clear();
   query << "all_cell_experiments" << info.expID;
   dbase->query( query );

   while ( dbase->next() )
   {
      QString letters( "SABCDEFGH" );
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

   query.clear();
   query << "get_solutionIDs" << info.expID;
   dbase->query( query );

   while ( dbase->next() )
   {
      QString solID = dbase->value( 0 ).toString();

      if ( ! solID.isEmpty()  &&  ! info.solutionIDs.contains( solID ) )
         info.solutionIDs << solID;
   }

   error.clear();

   return true;
}

// -------------------------------------------------------------- time state

bool US_DataPubCatalog::timeState( const Run& run, const QString& workDir,
                                   QString& tmstPath, QString& xdefPath )
{
   QString base = run.runID + ".time_state.tmst";

   if ( ! from_db )
   {
      QString dirPath = run.dirPath.isEmpty()
                        ? US_Settings::resultDir() + "/" + run.runID
                        : run.dirPath;
      tmstPath = dirPath + "/" + base;
      xdefPath = QString( tmstPath ).replace( ".tmst", ".xml" );

      return QFile( tmstPath ).exists();
   }

   if ( dbase == nullptr )  return false;

   int       tmstID = 0;
   int       expID  = run.id.toInt();
   QString   fname  = base;
   QString   xdefs;
   QString   cksum;
   QDateTime updated;

   US_TimeState::dbExamine( dbase, &tmstID, &expID, &fname, &xdefs, &cksum,
                            &updated );

   if ( tmstID <= 0 )  return false;

   if ( fname.isEmpty() )  fname = base;

   QDir().mkpath( workDir );

   tmstPath = workDir + "/" + fname;
   xdefPath = QString( tmstPath ).replace( ".tmst", ".xml" );

   if ( US_TimeState::dbDownload( dbase, tmstID, tmstPath ) != US_DB2::OK )
      return false;

   QFile xfile( xdefPath );

   if ( xfile.open( QIODevice::WriteOnly | QIODevice::Text ) )
   {
      QTextStream ts( &xfile );
      ts << xdefs;
      ts.flush();
      xfile.close();
   }

   return QFile( tmstPath ).exists();
}
