//! \file us_data_process.cpp

#include "us_data_process.h"
#include "us_convertio.h"
#include "us_data_model.h"
#include "us_datafiles.h"
#include "us_settings.h"
#include "us_util.h"

// class to process operatations on data:  upload/download/remove
US_DataProcess::US_DataProcess( US_DataModel* dmodel, QWidget* parent /*=0*/ )
   : QObject( parent )
{
   parentw          = parent;
   da_model         = dmodel;
   db               = da_model->dbase();
   dbg_level        = US_Settings::us_debug();
}

// perform a record upload to the database from local disk
int US_DataProcess::record_upload( int row )
{
DbgLv(1) << "REC_ULD: row" << row+1;
   int stat = 0;

   cdesc            = da_model->row_datadesc( row );

   if ( ! raw_ancestor_ok( row ) )
   {  // Cannot upload if ancestor Raw is not in both DB and Local
      errMsg = tr( "Cannot upload records whose ancestor"
                   " Raw record is not both DB and Local.\n"
                   "Use us_convert (Utilities->Convert Legacy Data)"
                   " to create or modify the ancestor Raw." );
      stat   = 3061;
      return stat;
   }

   if ( ! parent_in( row, US_DataModel::REC_DB ) )
   {  // A record goes to the DB after its parent, never before it
      errMsg = tr( "Cannot upload %1: its parent record is not in the"
                   " database.  Upload the parent first." )
               .arg( cdesc.label );
      stat   = NO_PARENT;
      return stat;
   }

   if ( unsafe_run( row ) )
      return UNSAFE_RUN;

   if ( ( cdesc.recState & US_DataModel::REC_DB ) != 0 )
   {  // A DB replace: is there anything to replace?
      int same = same_contents( cdesc );

      if ( same > 0 )
      {  // The DB holds what the file holds -- it may well have come
         // from this very file, so its date says nothing
         errMsg = tr( "%1 is already in sync: nothing to upload." )
                  .arg( cdesc.label );
         return IN_SYNC;
      }

      // The contents differ (or could not be compared):  a local copy that
      // is older than the DB copy does not replace it.  Both dates are
      // compared as UTC -- the catalogs read them so -- and a date that
      // cannot be read is no reason to refuse.
      QDateTime dtime  = US_DataModel::parse_date( cdesc.lastmodDate );
      QDateTime ftime  = US_DataModel::parse_date( cdesc.filemodDate );

      if ( dtime.isValid()  &&  ftime.isValid()  &&  ftime < dtime )
      {
         errMsg = tr( "The Local file record is older than the DB record,"
                      " and the two differ." );
         stat   = 3061;
         return stat;
      }
   }

   QStringList query;
   QString filepath = cdesc.filename;
   QString pathdir  = filepath.section( "/",  0, -2 );
   QString filename = filepath.section( "/", -1, -1 );
   int     idData   = cdesc.recordID;

   //if      ( cdesc.recType == 1 )
   if      ( cdesc.recType == 1  &&  cdesc.recType != 1 )  // never upload raw
   {  // upload a Raw record
      QString runID    = US_DataCatalog::runIdOfFile( filename, false );
      QString tripl    = filename.section( ".", -4, -2 )
                         .replace( ".", " / " );
DbgLv(1) << "REC_ULD:RAW: runID" << runID << "  tripl" << tripl;

DbgLv(1) << "REC_ULD:RAW: parentGUID" << cdesc.parentGUID;
      QString expGUID  = cdesc.parentGUID;

      if ( stat == 0 )
      {
         idData = cdesc.recordID;
         stat   = db->writeBlobToDB( filepath,
                                     QString( "upload_aucData" ),
                                     idData );
         errMsg = tr( "Raw upload writeBlobToDB() status %1" ).arg( stat );
         stat   = ( stat == 0 ) ? 0 : 3041;
      }
   }

   else if ( cdesc.recType == 2 )
   {  // upload an EditedData record
      QString runID    = US_DataCatalog::runIdOfFile( filename, true );
      // runID.editID.type -- the run ID may itself contain dots
      QString comment  = filename.section( ".", 0, -5 );
      QString editGUID = cdesc.dataGUID;
      QString rawGUID  = cdesc.parentGUID;
      QString editID   = QString::number( idData );

      // The mechanics are US_ConvertIO's; the create-then-always-update
      // sequence, the record-state codes and the table-model bookkeeping below
      // are this program's and are unchanged.
      int     rawDataID = -1;
      QString opError;

      if ( ( stat = US_ConvertIO::findRawDataId( db, rawGUID, rawDataID, opError ) ) != IUS_DB2::OK )
      {
         errMsg = tr( "Raw DB record for Edit could not be accessed: %1" ).arg( opError );
         stat   = 3052;
      }

      else
      {
         DbgLv( 1 ) << "editUpld: rawGUID" << rawGUID;
         DbgLv(1) << "editUpld: rawDataID" << rawDataID;

         US_ConvertIO::EditedDataRecord record;
         record.rawDataID = rawDataID;
         record.editGUID  = editGUID;
         record.label     = runID;
         record.filename  = filename;
         record.comment   = comment;

         if ( idData < 0 )
         {
            if ( US_ConvertIO::createEditedData( db, record, idData, opError ) != IUS_DB2::OK )
            {
               errMsg = tr( "Could not create the editedData record: %1" ).arg( opError );
               stat   = 3053;
            }
            else
            {
               editID = QString::number( idData );
               DbgLv( 1 ) << "editUpld: NEW idData" << idData << editID;
            }
         }

         if ( stat == US_DB2::OK )
         {
            if ( US_ConvertIO::updateEditedData( db, editID.toInt(), record, opError ) != IUS_DB2::OK )
            {
               errMsg = tr( "Could not update the editedData record: %1" ).arg( opError );
               stat   = 3054;
            }
            DbgLv( 1 ) << "editUpld: label" << runID;
         }

         if ( stat == US_DB2::OK )
         {
            stat           = US_ConvertIO::uploadEditedDataBlob( db, idData, filepath, opError );
            errMsg         = ( stat == 0 ) ? tr( "writeBlob Edited stat %1, idData %2" ).arg( stat ).arg( idData ) : opError;
            stat           = ( stat == 0 ) ? 0 : 3042;
            cdesc.recordID = idData;
         }
      }
   }

   else if ( cdesc.recType == 3 )
   {  // upload a Model record, from the file the row was read from
      US_Model model;
      int      lstat = filepath.isEmpty() ? US_DB2::NO_MODEL
                                          : model.load( filepath );

      if ( lstat != US_DB2::OK )
      {  // an empty model is not written in its place
         errMsg = tr( "The local model file could not be read (status %1),"
                      " so nothing was uploaded:\n " ).arg( lstat )
                  + filepath;
         stat   = 3046;
      }

      else
      {
         model.update_coefficients();    // fill in any missing coefficients
         stat   = model.write( db );     // store model to database
         errMsg = tr( "model write to DB status %1: %2" ).arg( stat )
                  .arg( model.message );
         stat   = ( stat == 0 )
                ? written_record_id( "get_modelID", model.modelGUID ) : 3043;
      }
DbgLv(1) << errMsg;
   }

   else if ( cdesc.recType == 4 )
   {  // upload a Noise record, from the file the row was read from
      US_Noise noise;
DbgLv(2) << "NOISE: filepath" << filepath;
      int      lstat = filepath.isEmpty() ? US_DB2::NO_NOISE
                                          : noise.load( filepath );

      if ( lstat != US_DB2::OK )
      {
         errMsg = tr( "The local noise file could not be read (status %1),"
                      " so nothing was uploaded:\n " ).arg( lstat )
                  + filepath;
         stat   = 3048;
      }

      else
      {
         stat   = noise.write( db );
         errMsg = tr( "noise write to DB status %1: %2" ).arg( stat )
                  .arg( noise.message );
         stat   = ( stat == 0 )
                ? written_record_id( "get_noiseID", noise.noiseGUID ) : 3044;
      }
   }

   else
   {  // *ERROR*:  invalid type
      errMsg = tr( "upload attempt with type=%1" ).arg( cdesc.recType );
      stat   = 3045;
   }

   if ( stat == 0 )
   {  // The record is in the DB now; the two copies are not compared yet
      cdesc.recState |= US_DataModel::REC_DB;
      cdesc.recState &= ~US_DataModel::HV_DET;
      cdesc.contents  = QString();
      da_model->change_datadesc( cdesc, row );
      da_model->record_changed ( row );
   }

   return stat;
}

// perform a record download from the database to local disk
int US_DataProcess::record_download( int row )
{
   int stat = 0;
   QStringList query;

   cdesc            = da_model->row_datadesc( row );

   if ( ! raw_ancestor_ok( row ) )
   {  // Cannot download if ancestor Raw is not in both DB and Local
      errMsg = tr( "Cannot download records whose ancestor"
                   " Raw record is not both DB and Local.\n"
                   "Use us_convert (Utilities->Convert Legacy Data)"
                   " to create or modify the ancestor Raw." );
      stat   = 3061;
      return stat;
   }

   if ( ! parent_in( row, US_DataModel::REC_LO ) )
   {  // A local record with no local parent would be an orphan on disk
      errMsg = tr( "Cannot download %1: its parent record is not on local"
                   " disk.  Download the parent first." )
               .arg( cdesc.label );
      stat   = NO_PARENT;
      return stat;
   }

   if ( unsafe_run( row ) )
      return UNSAFE_RUN;

   int idData       = cdesc.recordID;
   QString dataID   = QString::number( idData );
   QString filepath = cdesc.filename;
   QString dataGUID = cdesc.dataGUID;
   QString filename = filepath.section( "/", -1, -1 );

   // A run identifier may itself contain dots, so the run a file belongs to
   // is what is left after the fixed trailing parts of the name
   QString runID    = US_DataCatalog::runIdOfFile(
                      filename, cdesc.recType == US_DataModel::EDIT );

   filepath         = ( filepath == filename ) ?
                     US_Settings::resultDir() + "/" + runID + "/" + filename :
                     filepath;

   QDir dirp;
   QString filedir  = filepath.section( "/", 0, -2 );

   if ( ! dirp.exists( filedir ) )
   {
      if ( ! dirp.mkpath( filedir ) )
      {
         stat    = 3091;
         errMsg  = tr( "Unable to create download directory\n" )
                   + filedir;
         return stat;
      }
   }

   //if      ( cdesc.recType == 1 )
   if      ( cdesc.recType == 1  &&  cdesc.recType != 1 )  // never download raw
   {  // download a Raw record
      QString opError;
      stat   = US_ConvertIO::downloadRawDataBlob( db, idData, filepath,
                                                  opError );
      if ( stat != 0 )
         errMsg = tr( "Raw download status %1" ).arg( stat )
                  + "\n " + filepath;
   }

   else if ( cdesc.recType == 2 )
   {  // download an EditedData record
      QString opError;
      stat   = US_ConvertIO::downloadEditedDataBlob( db, idData, filepath,
                                                     opError );
      if ( stat != 0 )
         errMsg = tr( "Edited download status %1: %2" ).arg( stat )
                  .arg( opError ) + "\n " + filepath;
   }

   else if ( cdesc.recType == 3 )
   {  // download a Model record
//QTime timer3;
//timer3.start();
      filepath = get_model_filename( &cdesc );
//qDebug() << "DwnLd:  model time1" << timer3.elapsed();
      QStringList query;
      query << "get_model_info" << dataID;
      db->query( query );
      stat       = db->lastErrno();

      if ( stat == US_DB2::OK  &&  ! db->next() )
         stat       = US_DB2::NOROWS;

      if ( stat == US_DB2::OK )
      {
//qDebug() << "DwnLd:   model time2" << timer3.elapsed();
         // The model XML is UTF-8, whatever the locale, and is written as
         // the server holds it, so the two copies compare as equal
         QByteArray contents = db->value( 2 ).toString().toUtf8();
         QFile fileo( filepath );
         if ( fileo.open( QIODevice::WriteOnly )  &&
              fileo.write( contents ) == contents.size()  &&
              fileo.flush() )
         {
            fileo.close();
            cdesc.filename = filepath;
//qDebug() << "DwnLd:   model time3" << timer3.elapsed();
         }

         else
         {
            errMsg = tr( "Model file could not be written\n " ) + filepath;
            stat   = 3023;  // download write error
         }
      }

      else
      {
         errMsg = tr( "Model load for download, status %1" ).arg( stat );
         stat   = 3033;     // download read error
      }
   }

   else if ( cdesc.recType == 4 )
   {  // download a Noise record
      US_Noise noise;

      filepath = get_noise_filename( &cdesc );
      stat     = noise.load( dataID, db );

      if ( stat == US_DB2::OK )
      {
         stat     = noise.write( filepath );

         if ( stat != US_DB2::OK )
         {
            errMsg = tr( "Noise write for download, status %1" ).arg( stat );
            stat   = 3024;  // download write error
         }

         else
            cdesc.filename = filepath;
      }

      else
      {
         errMsg = tr( "Noise load for download, status %1" ).arg( stat );
         stat   = 3034;     // download read error
      }
   }

   else
   {  // *ERROR*:  invalid type
      errMsg = tr( "download attempt with type=%1" ).arg( cdesc.recType );
      stat   = 3015;
   }

   if ( stat == 0 )
   {  // The record is on local disk now, in the file just written; the two
      // copies are not compared yet
      cdesc.recState |= US_DataModel::REC_LO;
      cdesc.recState &= ~US_DataModel::HV_DET;
      cdesc.contents  = QString();

      if ( cdesc.recType == US_DataModel::RAW  ||
           cdesc.recType == US_DataModel::EDIT )
         cdesc.filename = filepath;

      da_model->change_datadesc( cdesc, row );
      da_model->record_changed ( row );
   }

   return stat;
}
// perform a record remove from the database
int US_DataProcess::record_remove_db( int row )
{
   int stat = 0;
   QStringList query;

   cdesc            = da_model->row_datadesc( row );
   int     irecID   = cdesc.recordID;
   QString dataID   = QString::number( irecID );

   if      ( cdesc.recType == 1 )
   { // remove a Raw record
      query.clear();
      query << "delete_rawData" << QString::number( cdesc.parentID );
DbgLv(1) << "REC_RMV: exp ID" << cdesc.parentID;

      if ( ( stat = db->statusQuery( query ) ) != 0 )
      {
         errMsg = tr( "delete_rawData status=%1" ).arg( stat );
         stat   = 2011;
      }
      else
         irecID = -1;
   }

   else if ( cdesc.recType != US_DataModel::RAW  &&  irecID < 0 )
   {  // no database ID to remove by: nothing else may be removed instead
      errMsg = tr( "%1 has no database ID to remove it by; scan again." )
               .arg( cdesc.label );
      stat   = 2016;
   }

   else if ( cdesc.recType == 2 )
   {  // remove an EditedData record
      // The noise of another edit's model recorded against this edit is
      // not removed with it, and the edit cannot go while it is there
      QMap< int, QString > foreign;

      if ( ! foreign_noise_of_edit( irecID, foreign ) )
         return 2017;

      for ( auto it = foreign.constBegin(); it != foreign.constEnd(); ++it )
      {
         query.clear();
         query << "delete_noise" << QString::number( it.key() );

         if ( ( stat = db->statusQuery( query ) ) != 0 )
         {
            errMsg = tr( "%1 could not be removed: noise %2 of another edit's"
                         " model was computed from it and could not be"
                         " removed first (delete_noise status %3: %4)" )
                     .arg( cdesc.label ).arg( it.value() ).arg( stat )
                     .arg( db->lastError() );
            return 2018;
         }

         // Its rows -- under that other model -- are gone from the database
         for ( int nrow = 0; nrow < da_model->recCount(); nrow++ )
         {
            US_DataModel::DataDesc ndesc = da_model->row_datadesc( nrow );

            if ( ndesc.recType == US_DataModel::NOISE  &&
                 ndesc.recordID == it.key()  &&
                 ( ndesc.recState & US_DataModel::REC_DB ) != 0 )
               mark_removed_from_db( nrow );
         }
      }

      cdesc = da_model->row_datadesc( row );   // as it is now
      query.clear();
      query << "delete_editedData" << dataID;
DbgLv(1) << "EDT_RMV:" << query;

      if ( ( stat = db->statusQuery( query ) ) != 0 )
      {
         errMsg = tr( "delete_editedData status=%1: %2" ).arg( stat )
                  .arg( db->lastError() )
                  + tr( "\nNoise of a model of another investigator, or of a"
                        " model this window does not list, may have been"
                        " computed from this edit; the database keeps the"
                        " edit while such noise refers to it." );
DbgLv(1) << "EDT_RMV:   stat" << stat << db->lastError() << db->lastErrno();
DbgLv(1) << "EDT_RMV:   dataID" << dataID << "errMsg:" << errMsg;
         stat   = 2012;
      }
      else
         irecID = -1;
DbgLv(1) << "EDT_RMV:    stat" << stat;
   }

   else if ( cdesc.recType == 3 )
   {  // remove a Model record
      query.clear();
      query << "delete_model" << dataID;

      if ( ( stat = db->statusQuery( query ) ) != 0 )
      {
         errMsg = tr( "delete_model status=%1: %2" ).arg( stat )
                  .arg( db->lastError() );
DbgLv(1) << "MOD_RMV:   dataID" << dataID << "errMsg:" << errMsg;
DbgLv(1) << "MOD_RMV:    lastErr" << db->lastError() << db->lastErrno();
         stat   = 2013;
      }
      else
         irecID = -1;
   }

   else if ( cdesc.recType == 4 )
   {  // remove a Noise record
      query.clear();
      query << "delete_noise" << dataID;

      if ( ( stat = db->statusQuery( query ) ) != 0 )
      {
         errMsg = tr( "delete_noise status=%1: %2" ).arg( stat )
                  .arg( db->lastError() );
DbgLv(1) << "NOI_RMV:   dataID" << dataID << "errMsg:" << errMsg;
DbgLv(1) << "NOI_RMV:    lastErr" << db->lastError() << db->lastErrno();
         stat   = 2014;
      }
      else
         irecID = -1;
   }

   else
   {  // *ERROR*:  invalid type
      errMsg = tr( "DB remove attempt for type=%1" ).arg( cdesc.recType );
      stat   = 2015;
   }

   if ( stat == 0 )
   {  // Gone from the DB:  local only now, or gone altogether
      mark_removed_from_db( row );
   }

   return stat;
}

// Mark a row as gone from the database: local only now, or gone altogether
void US_DataProcess::mark_removed_from_db( int row )
{
   US_DataModel::DataDesc desc = da_model->row_datadesc( row );

   desc.recordID  = -1;
   desc.recState &= ~( US_DataModel::REC_DB | US_DataModel::HV_DET );
   desc.contents  = QString();

   if ( ( desc.recState & US_DataModel::REC_LO ) == 0 )
      desc.recState  = US_DataModel::NOSTAT;

   da_model->change_datadesc( desc, row );
   da_model->record_changed ( row );
}

/* The noise recorded against an edit whose model belongs to another edit.

   delete_editedData removes the edit's models and their noise, wherever
   that noise was recorded; it does not remove noise recorded against the
   edit for a model of another edit (a global fit, say), and while such
   noise is there the edit cannot be removed.  These are the investigator's
   records, as the listings give them.
*/
bool US_DataProcess::foreign_noise_of_edit( int editID,
                                            QMap< int, QString >& noise )
{
   noise.clear();

   QString     invID = QString::number( US_Settings::us_inv_ID() );
   QString     edtID = QString::number( editID );
   QStringList query;
   QSet< int > models;

   query << "get_model_desc_by_editID" << invID << edtID;
   db->query( query );
   int status = db->lastErrno();

   if ( status != IUS_DB2::OK  &&  status != IUS_DB2::NOROWS )
   {
      errMsg = tr( "The models of edit %1 could not be listed: %2" )
               .arg( editID ).arg( db->lastError() );
      return false;
   }

   while ( status == IUS_DB2::OK  &&  db->next() )
      models.insert( db->value( 0 ).toInt() );

   query.clear();
   query << "get_noise_desc_by_editID" << invID << edtID;
   db->query( query );
   status = db->lastErrno();

   if ( status != IUS_DB2::OK  &&  status != IUS_DB2::NOROWS )
   {
      errMsg = tr( "The noise of edit %1 could not be listed: %2" )
               .arg( editID ).arg( db->lastError() );
      return false;
   }

   // noiseID, noiseGUID, editedDataID, modelID, ...
   while ( status == IUS_DB2::OK  &&  db->next() )
   {
      if ( db->value( 2 ).toInt() != editID )             continue;
      if ( models.contains( db->value( 3 ).toInt() ) )    continue;

      noise.insert( db->value( 0 ).toInt(), db->value( 1 ).toString() );
   }

   return true;
}

// The database ID a model or noise row holds after its record is written.
//
// The write looked the record up by its GUID and updated it, or created one
// when there was none.  A replace keeps the row's ID -- the GUID is unique,
// so the row already names the record that was updated; a create asks for
// the ID by GUID.  lastInsertID() says nothing after an update, and is not
// asked.
int US_DataProcess::written_record_id( const QString& proc,
                                       const QString& guid )
{
   if ( ( cdesc.recState & US_DataModel::REC_DB ) != 0  &&
        cdesc.recordID >= 0  &&
        guid.compare( cdesc.dataGUID, Qt::CaseInsensitive ) == 0 )
      return 0;

   QStringList query;
   query << proc << guid;
   db->query( query );

   int status = db->lastErrno();

   if ( status == IUS_DB2::OK  &&  db->next() )
   {
      int id = db->value( 0 ).toInt();

      if ( id > 0 )
      {
         cdesc.recordID = id;
         cdesc.dataGUID = guid;
         return 0;
      }
   }

   errMsg = tr( "%1 was written to the database, but its database ID could"
                " not be read (%2: %3); scan again before acting on it." )
            .arg( cdesc.label ).arg( proc ).arg( db->lastError() );
   return 3047;
}

// perform a record remove from local disk
int US_DataProcess::record_remove_local( int row )
{
   int stat = 0;
   cdesc            = da_model->row_datadesc( row );

   QString filepath = cdesc.filename;
   QString filename = filepath.section( "/", -1, -1 );
   QFile   file( filepath );

   if ( file.exists() )
   {  // the file to remove does exist

      if ( file.remove() )
      {  // the remove was successful

         if ( ( cdesc.recState & US_DataModel::REC_DB ) == 0 )
         {  // it was local-only, so now it's a dummy
            cdesc.recState  = US_DataModel::NOSTAT;
         }

         else
         {  // it was on both, so now it's db-only
            cdesc.recState &= ~( US_DataModel::REC_LO | US_DataModel::HV_DET );
            cdesc.contents  = QString();
         }
      }

      else
      {  // an error occurred in removing
         errMsg = tr( "File remove error,\n" ) + filepath;
         stat   = 1000;
      }
   }

   else
   {  // file did not exist
      errMsg = tr( "*ERROR* attempt to remove non-existent file\n" ) + filepath;
      stat   = 2000;
   }

   if ( stat == 0 )
   {
      da_model->change_datadesc( cdesc, row );
      da_model->record_changed ( row );
   }

   return stat;
}

// Get next model file name
QString US_DataProcess::get_model_filename( US_DataModel::DataDesc* ddesc )
{
   QString path = ddesc->filename;

   if ( !path.isEmpty()  &&  QFile( path ).exists() )
      return path;

   if ( ( ddesc->recState & US_DataModel::REC_LO ) == 0 )
      return get_model_filename( "" );
   else
      return get_model_filename( ddesc->dataGUID );
}

// Get next model file name
QString US_DataProcess::get_model_filename( QString guid )
{
   QString path;

   if ( ! US_Model::model_path( path ) )
      return "";

   return US_DataFiles::get_filename( path, guid, "M", "model", "modelGUID" );
}


// Get next noise file name
QString US_DataProcess::get_noise_filename( US_DataModel::DataDesc* ddesc )
{
   QString path = ddesc->filename;

   if ( !path.isEmpty()  &&  QFile( path ).exists() )
      return path;

   if ( ( ddesc->recState & US_DataModel::REC_LO ) == 0 )
      return get_noise_filename( "" );
   else
      return get_noise_filename( ddesc->dataGUID );
}

// Get next noise file name
QString US_DataProcess::get_noise_filename( QString guid )
{
   QString path  = US_Settings::dataDir() + "/noises";
   QDir    dir;

   if ( ! dir.exists( path ) )
   {
      if ( ! dir.mkpath( path ) )
         return "";
   }

   return US_DataFiles::get_filename( path, guid, "N", "noise", "noiseGUID" );
}

// Return the last error message
QString US_DataProcess::lastError( )
{
   return errMsg;
}

// Append to last error message a notice of errors on some actions
void US_DataProcess::partialError( int nacterrs, int nactions )
{
   errMsg += tr( "\n\n%1 error(s) on %2 attempted actions.\n" )
             .arg( nacterrs ).arg( nactions );
}

// Append a string to the last error message
void US_DataProcess::appendError( QString aperrMsg )
{
   errMsg += aperrMsg;
}

// True when a record's parent is in the given place.  An edit's parent is
// its raw record, which raw_ancestor_ok() looks at; a filler is in neither
// place.  Uploads and downloads go top down, so a parent acted on in the
// same action is in place by the time its children are reached.
bool US_DataProcess::parent_in( int row, int place )
{
   US_DataModel::DataDesc desc = da_model->row_datadesc( row );

   if ( desc.recType <= US_DataModel::EDIT )   return true;

   int prow = da_model->parent_row( row );

   if ( prow < 0 )   return false;

   return ( ( da_model->row_datadesc( prow ).recState & place ) != 0 );
}

// True, with the message set, when the record's run must not be written to
bool US_DataProcess::unsafe_run( int row )
{
   US_DataModel::RunEntry entry =
      da_model->run_entry( da_model->run_of_row( row ) );

   if ( ! entry.unsafe )   return false;

   errMsg = tr( "Run %1 is marked \"(!)\": %2\n"
                "Nothing is uploaded or downloaded for it until its local"
                " directory is named after it and is the only one holding"
                " it." ).arg( entry.runID ).arg( entry.problem );
   return true;
}

// Whether the two copies of a record hold the same thing
int US_DataProcess::same_contents( const US_DataModel::DataDesc& desc )
{
   QString dbd;
   QString lod;

   if ( ( desc.recState & US_DataModel::HV_DET ) != 0 )
   {  // Compared already: the database copy is the one compared then
      dbd = desc.contents.section( " ", 0, 1 ).simplified();
   }

   else
   {  // Compare them now, this record only
      QString error;

      if ( ! US_DataModel::db_digest( db, desc, dbd, error ) )
         dbd.clear();
   }

   // The file may have changed since the comparison: it is what would be
   // uploaded, so it is what is hashed
   lod = US_DataModel::file_digest( desc.filename );

   if ( dbd.isEmpty()  ||  lod.isEmpty()  ||
        dbd.startsWith( "-" )  ||  lod.startsWith( "-" ) )
      return -1;

   return ( dbd == lod ) ? 1 : 0;
}

// Test for ancestor Raw record in both DB and local

bool US_DataProcess::raw_ancestor_ok( int row )
{
   bool isOK = false;
   int  jrow = row;
   int  bSrc = US_DataModel::REC_DB | US_DataModel::REC_LO;

   // Walk up the parents -- not up the rows, which belong to other
   // experiments above the first record of this one -- to the Raw record
   while ( ( jrow = da_model->parent_row( jrow ) ) >= 0 )
   {
      US_DataModel::DataDesc ddesc = da_model->row_datadesc( jrow );

      if ( ddesc.recType == US_DataModel::RAW )
      {  // This is the Raw ancestor of the given record
         if ( ( ddesc.recState & bSrc ) == bSrc )
         {  // If both DB and Local, mark as OK for upload/download
            isOK = true;
         }
         break;
      }
   }

   return isOK;
}

