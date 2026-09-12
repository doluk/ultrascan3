//! \file us_datapub_catalog.cpp
#include "us_datapub_catalog.h"
#include "us_datapub_hash.h"

#include "us_settings.h"
#include "us_util.h"
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
}

US_DataPubCatalog::~US_DataPubCatalog()
{
   if ( dbase != nullptr )  delete dbase;

   dbase = nullptr;
}

bool US_DataPubCatalog::open( bool fromDb, const QString& dbPassword,
                              QString& error )
{
   from_db = fromDb;
   inv_id  = US_Settings::us_inv_ID();

   if ( dbase != nullptr )
   {
      delete dbase;
      dbase = nullptr;
   }

   if ( ! from_db )
   {
      QString rdir = US_Settings::resultDir();

      if ( ! QDir( rdir ).exists() )
      {
         error = QObject::tr( "The local results directory does not exist: %1" )
                 .arg( rdir );
         return false;
      }

      error.clear();
      return true;
   }

   dbase = new US_DB2( dbPassword );

   if ( dbase->lastErrno() != US_DB2::OK )
   {
      error = QObject::tr( "Cannot connect to the database: %1" )
              .arg( dbase->lastError() );
      delete dbase;
      dbase = nullptr;
      return false;
   }

   error.clear();

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

QList< US_DataPubCatalog::Run > US_DataPubCatalog::runs(
      const QString& projectGUID, QString& error )
{
   return from_db ? runsDb( projectGUID, error ) : runsDisk( projectGUID, error );
}

QList< US_DataPubCatalog::Run > US_DataPubCatalog::runsDb(
      const QString& projectGUID, QString& error )
{
   QList< Run > list;

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return list;
   }

   QString     invID = QString::number( inv_id );
   QStringList query;
   query << "get_experiment_desc" << invID;
   dbase->query( query );

   if ( dbase->lastErrno() != US_DB2::OK  &&
        dbase->lastErrno() != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return list;
   }

   QList< Run > headers;

   while ( dbase->next() )
   {
      Run run;
      run.id      = dbase->value( 0 ).toString();
      run.runID   = dbase->value( 1 ).toString();
      run.expType = dbase->value( 2 ).toString();
      run.label   = dbase->value( 4 ).toString();
      run.date    = dbase->value( 5 ).toString();

      headers << run;
   }

   // Fill in the experiment GUID and the owning project of each run.  The
   // project descriptions are cached: a project usually owns many runs.
   QMap< QString, QString > projGUIDs;
   QMap< QString, QString > projDescs;

   for ( int ii = 0; ii < headers.size(); ii++ )
   {
      Run run = headers[ ii ];

      query.clear();
      query << "get_experiment_info_by_runID" << run.runID << invID;
      dbase->query( query );

      if ( dbase->next() )
      {
         run.projectID = dbase->value(  0 ).toString();
         run.id        = dbase->value(  1 ).toString();
         run.guid      = dbase->value(  2 ).toString();
         run.expType   = dbase->value(  8 ).toString();
         run.label     = dbase->value( 10 ).toString();
         run.date      = dbase->value( 13 ).toString();
      }

      if ( ! projGUIDs.contains( run.projectID ) )
      {
         QString pguid;
         QString pdesc;

         query.clear();
         query << "get_project_info" << run.projectID;
         dbase->query( query );

         if ( dbase->next() )
         {
            pguid = dbase->value(  1 ).toString();
            pdesc = dbase->value( 10 ).toString();
         }

         projGUIDs.insert( run.projectID, pguid );
         projDescs.insert( run.projectID, pdesc );
      }

      run.projectGUID = projGUIDs.value( run.projectID );
      run.projectDesc = projDescs.value( run.projectID );

      if ( ! projectGUID.isEmpty()  &&  run.projectGUID != projectGUID )
         continue;

      list << run;
   }

   error.clear();

   return list;
}

QList< US_DataPubCatalog::Run > US_DataPubCatalog::runsDisk(
      const QString& projectGUID, QString& error )
{
   QList< Run > list;
   QString      rdir    = US_Settings::resultDir();
   QStringList  subdirs = QDir( rdir ).entryList(
                          QDir::AllDirs | QDir::NoDotAndDotDot, QDir::Name );

   for ( int ii = 0; ii < subdirs.size(); ii++ )
   {
      QString     subdir   = rdir + "/" + subdirs[ ii ];
      QStringList aucfiles = QDir( subdir ).entryList(
                             QStringList( "*.auc" ), QDir::Files, QDir::Name );

      if ( aucfiles.isEmpty() )  continue;

      QString aucbase = aucfiles[ 0 ];
      Run     run;
      run.runID   = aucbase.section( ".",  0, -6 );
      run.runType = aucbase.section( ".", -5, -5 );
      run.dirPath = subdir;
      run.label   = run.runID;

      ExpInfo info;

      if ( info.readFromFile( expFilePath( run ) ) )
      {
         run.id          = info.expID;
         run.guid        = info.expGUID;
         run.expType     = info.expType;
         run.projectID   = info.projectID;
         run.projectGUID = info.projectGUID;
         run.projectDesc = info.projectDesc;

         if ( ! info.label.isEmpty() )  run.label = info.label;
         if ( ! info.runID.isEmpty() )  run.runID = info.runID;
      }

      run.date = US_Util::toUTCDatetimeText(
                 QFileInfo( subdir + "/" + aucbase ).lastModified().toUTC()
                 .toString( Qt::ISODate ), true );

      if ( ! projectGUID.isEmpty()  &&  run.projectGUID != projectGUID )
         continue;

      list << run;
   }

   error.clear();

   return list;
}

bool US_DataPubCatalog::runByID( const QString& runID, Run& found,
                                 QString& error )
{
   QList< Run > list = runs( QString(), error );

   for ( int ii = 0; ii < list.size(); ii++ )
   {
      if ( list[ ii ].runID != runID )  continue;

      found = list[ ii ];
      return true;
   }

   if ( error.isEmpty() )
      error = QObject::tr( "No run \"%1\" was found in the %2" ).arg( runID )
              .arg( from_db ? QObject::tr( "database" )
                            : QObject::tr( "local results directory" ) );

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
   return from_db ? loadRunDetailsDb( run, error )
                  : loadRunDetailsDisk( run, error );
}

bool US_DataPubCatalog::loadRunDetailsDb( Run& run, QString& error )
{
   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return false;
   }

   run.raws.clear();

   QStringList query;
   QStringList rawIDs;
   QStringList rawNames;

   query << "get_rawDataIDs" << run.id;
   dbase->query( query );

   if ( dbase->lastErrno() != US_DB2::OK  &&
        dbase->lastErrno() != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return false;
   }

   while ( dbase->next() )
   {
      rawIDs   << dbase->value( 0 ).toString();
      rawNames << dbase->value( 2 ).toString().replace( "\\", "/" )
                  .section( "/", -1, -1 );
   }

   for ( int ii = 0; ii < rawIDs.size(); ii++ )
   {
      Raw raw;
      raw.id       = rawIDs  [ ii ];
      raw.filename = rawNames[ ii ];
      raw.runID    = run.runID;
      raw.dataType = raw.filename.section( ".", -5, -5 );
      raw.triple   = raw.filename.section( ".", -4, -2 );

      query.clear();
      query << "get_rawData" << raw.id;
      dbase->query( query );

      if ( dbase->next() )
         raw.guid  = dbase->value( 0 ).toString();

      QStringList edtIDs;

      query.clear();
      query << "get_editedDataIDs" << raw.id;
      dbase->query( query );

      while ( dbase->next() )
         edtIDs << dbase->value( 0 ).toString();

      for ( int jj = 0; jj < edtIDs.size(); jj++ )
      {
         Edit edit;
         edit.id      = edtIDs[ jj ];
         edit.runID   = run.runID;
         edit.rawGUID = raw.guid;

         query.clear();
         query << "get_editedData" << edit.id;
         dbase->query( query );

         if ( ! dbase->next() )  continue;

         edit.guid     = dbase->value( 1 ).toString();
         edit.filename = dbase->value( 3 ).toString().replace( "\\", "/" )
                         .section( "/", -1, -1 );
         edit.editID   = edit.filename.section( ".", -6, -6 );
         edit.triple   = edit.filename.section( ".", -4, -2 );
         edit.label    = edit.editID + " (" + edit.triple + ")";

         raw.edits << edit;
      }

      run.raws << raw;
   }

   error.clear();

   return true;
}

bool US_DataPubCatalog::loadRunDetailsDisk( Run& run, QString& error )
{
   run.raws.clear();

   if ( run.dirPath.isEmpty() )
      run.dirPath = US_Settings::resultDir() + "/" + run.runID;

   QDir        rundir( run.dirPath );
   QStringList aucfiles = rundir.entryList( QStringList( "*.auc" ),
                                            QDir::Files, QDir::Name );

   if ( aucfiles.isEmpty() )
   {
      error = QObject::tr( "No .auc files were found in %1" ).arg( run.dirPath );
      return false;
   }

   QStringList edtfiles = rundir.entryList( QStringList( "*.xml" ),
                                            QDir::Files, QDir::Name );

   for ( int ii = 0; ii < aucfiles.size(); ii++ )
   {
      QString aucbase = aucfiles[ ii ];
      Raw     raw;
      raw.filename = aucbase;
      raw.path     = run.dirPath + "/" + aucbase;
      raw.runID    = aucbase.section( ".",  0, -6 );
      raw.dataType = aucbase.section( ".", -5, -5 );
      raw.triple   = aucbase.section( ".", -4, -2 );

      QMap< QString, QString > attrs = peekAttributes( raw.path, "rawData" );
      raw.guid = attrs.value( "guid" );

      if ( raw.guid.isEmpty() )
      {
         // .auc files are binary; the GUID lives in the edit files that
         // point at them, so fall back to the first edit of this triple.
         for ( int jj = 0; jj < edtfiles.size(); jj++ )
         {
            QString edtbase = edtfiles[ jj ];

            if ( edtbase.section( ".", -4, -2 ) != raw.triple )    continue;
            if ( edtbase.section( ".", -5, -5 ) != raw.dataType )  continue;

            QMap< QString, QString > eattrs = peekAttributes(
                  run.dirPath + "/" + edtbase, "rawDataGUID" );
            raw.guid = eattrs.value( "value" );

            if ( ! raw.guid.isEmpty() )  break;
         }
      }

      for ( int jj = 0; jj < edtfiles.size(); jj++ )
      {
         QString edtbase = edtfiles[ jj ];

         // Edit file names carry six dot-separated parts before the
         // extension; the run's experiment XML carries two.
         if ( edtbase.count( "." ) < 6 )                        continue;
         if ( edtbase.section( ".", -4, -2 ) != raw.triple )    continue;
         if ( edtbase.section( ".", -5, -5 ) != raw.dataType )  continue;

         Edit edit;
         edit.filename = edtbase;
         edit.path     = run.dirPath + "/" + edtbase;
         edit.runID    = edtbase.section( ".",  0, -7 );
         edit.editID   = edtbase.section( ".", -6, -6 );
         edit.triple   = raw.triple;
         edit.rawGUID  = raw.guid;

         QMap< QString, QString > eattrs = peekAttributes( edit.path,
                                                           "editGUID" );
         edit.guid     = eattrs.value( "value" );
         edit.label    = edit.editID + " (" + edit.triple + ")";

         raw.edits << edit;
      }

      run.raws << raw;
   }

   error.clear();

   return true;
}

// ------------------------------------------------------------------ models

QList< US_DataPubCatalog::Model > US_DataPubCatalog::models(
      const QList< Run >& runList, QString& error )
{
   return from_db ? modelsDb( runList, error ) : modelsDisk( runList, error );
}

QList< US_DataPubCatalog::Model > US_DataPubCatalog::modelsDb(
      const QList< Run >& runList, QString& error )
{
   QList< Model > list;

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return list;
   }

   QString     invID = QString::number( inv_id );
   QStringList query;
   QStringList seen;

   for ( int ii = 0; ii < runList.size(); ii++ )
   {
      const Run& run = runList[ ii ];

      for ( int jj = 0; jj < run.raws.size(); jj++ )
      {
         const Raw& raw = run.raws[ jj ];

         for ( int kk = 0; kk < raw.edits.size(); kk++ )
         {
            const Edit& edit = raw.edits[ kk ];

            if ( edit.id == "-1"  ||  edit.id.isEmpty() )  continue;

            query.clear();
            query << "get_model_desc_by_editID" << invID << edit.id;
            dbase->query( query );

            while ( dbase->next() )
            {
               Model model;
               model.id          = dbase->value( 0 ).toString();
               model.guid        = dbase->value( 1 ).toString();
               model.description = dbase->value( 2 ).toString();
               model.editGUID    = dbase->value( 5 ).toString();

               if ( model.editGUID.isEmpty() )  model.editGUID = edit.guid;
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

QList< US_DataPubCatalog::Model > US_DataPubCatalog::modelsDisk(
      const QList< Run >& runList, QString& error )
{
   QList< Model > list;
   QStringList    editGUIDs;

   for ( int ii = 0; ii < runList.size(); ii++ )
      for ( int jj = 0; jj < runList[ ii ].raws.size(); jj++ )
         for ( int kk = 0; kk < runList[ ii ].raws[ jj ].edits.size(); kk++ )
            editGUIDs << runList[ ii ].raws[ jj ].edits[ kk ].guid;

   QString     path  = US_Settings::dataDir() + "/models";
   QStringList files = QDir( path ).entryList( QStringList( "M???????.xml" ),
                                               QDir::Files, QDir::Name );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QString                  fpath = path + "/" + files[ ii ];
      QMap< QString, QString > attrs = peekAttributes( fpath, "model" );

      if ( attrs.isEmpty() )  continue;

      QString editGUID = attrs.value( "editGUID" );

      if ( ! editGUIDs.contains( editGUID ) )  continue;

      Model model;
      model.guid        = attrs.value( "modelGUID" );
      model.description = attrs.value( "description" );
      model.editGUID    = editGUID;
      model.filename    = files[ ii ];

      list << model;
   }

   error.clear();

   return list;
}

// ------------------------------------------------------------------ noises

QList< US_DataPubCatalog::Noise > US_DataPubCatalog::noises(
      const QList< Model >& modelList, QString& error )
{
   return from_db ? noisesDb( modelList, error )
                  : noisesDisk( modelList, error );
}

QList< US_DataPubCatalog::Noise > US_DataPubCatalog::noisesDb(
      const QList< Model >& modelList, QString& error )
{
   QList< Noise > list;

   if ( dbase == nullptr )
   {
      error = QObject::tr( "No database connection" );
      return list;
   }

   QStringList modelGUIDs;

   for ( int ii = 0; ii < modelList.size(); ii++ )
      modelGUIDs << modelList[ ii ].guid;

   // The database offers noise lookups by investigator and by edit, but not
   // by model, so the investigator's noise records are listed once and
   // filtered on the model GUIDs of the selected models.
   QStringList query;
   query << "get_noise_desc" << QString::number( inv_id );
   dbase->query( query );

   if ( dbase->lastErrno() != US_DB2::OK  &&
        dbase->lastErrno() != US_DB2::NOROWS )
   {
      error = dbase->lastError();
      return list;
   }

   QStringList seen;

   while ( dbase->next() )
   {
      QString modelGUID = dbase->value( 5 ).toString();
      int     modelx    = modelGUIDs.indexOf( modelGUID );

      if ( modelx < 0 )  continue;

      Noise noise;
      noise.id          = dbase->value( 0 ).toString();
      noise.guid        = dbase->value( 1 ).toString();
      noise.noiseType   = dbase->value( 4 ).toString();
      noise.modelGUID   = modelGUID;
      noise.description = dbase->value( 9 ).toString();
      noise.editGUID    = modelList[ modelx ].editGUID;

      if ( seen.contains( noise.guid ) )  continue;

      seen << noise.guid;
      list << noise;
   }

   error.clear();

   return list;
}

QList< US_DataPubCatalog::Noise > US_DataPubCatalog::noisesDisk(
      const QList< Model >& modelList, QString& error )
{
   QList< Noise > list;
   QStringList    modelGUIDs;

   for ( int ii = 0; ii < modelList.size(); ii++ )
      modelGUIDs << modelList[ ii ].guid;

   QString     path  = US_Settings::dataDir() + "/noises";
   QStringList files = QDir( path ).entryList( QStringList( "N???????.xml" ),
                                               QDir::Files, QDir::Name );

   for ( int ii = 0; ii < files.size(); ii++ )
   {
      QString                  fpath = path + "/" + files[ ii ];
      QMap< QString, QString > attrs = peekAttributes( fpath, "noise" );

      if ( attrs.isEmpty() )  continue;

      QString modelGUID = attrs.value( "modelGUID" );
      int     modelx    = modelGUIDs.indexOf( modelGUID );

      if ( modelx < 0 )  continue;

      Noise noise;
      noise.guid        = attrs.value( "noiseGUID" );
      noise.description = attrs.value( "description" );
      noise.noiseType   = attrs.value( "type" );
      noise.modelGUID   = modelGUID;
      noise.editGUID    = modelList[ modelx ].editGUID;
      noise.filename    = files[ ii ];

      list << noise;
   }

   error.clear();

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
