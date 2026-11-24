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

   if ( stat == 3047 )
   {  // Written, but under an ID that could not be read: the row says it
      // is in the database, with no ID to act on it by until a new scan
      cdesc.recordID  = -1;
      cdesc.recState |= US_DataModel::REC_DB;
      cdesc.recState &= ~US_DataModel::HV_DET;
      cdesc.contents  = QString();
      da_model->change_datadesc( cdesc, row );
      da_model->record_changed ( row );
   }

   if ( stat == 0 )
   {  // The record is in the DB now; the two copies are not compared yet
      cdesc.recState |= US_DataModel::REC_DB;
      cdesc.recState &= ~US_DataModel::HV_DET;
      cdesc.contents  = QString();
      da_model->change_datadesc( cdesc, row );
      da_model->record_changed ( row );
   }

   return note_lost( stat );
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

   return note_lost( stat );
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
   {  // remove an EditedData record, with the noise of other edits' models
      // recorded against it
      if ( ( stat = remove_edit( row ) ) == 0 )
         irecID = -1;
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

   return note_lost( stat );
}

// Mark a row as gone from the database: local only now, or gone altogether
void US_DataProcess::mark_removed_from_db( int row )
{
   US_DataModel::DataDesc desc = da_model->row_datadesc( row );

   // No later merge from the catalogs shows it, or what went with it
   if ( ( desc.recState & US_DataModel::REC_DB ) != 0 )
      da_model->removed_from_db( desc.recType, desc.recordID );

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
   that noise was recorded; an older one does not remove noise recorded
   against the edit for a model of another edit (a global fit, say), and
   while such noise is there the edit cannot be removed.  These are the
   investigator's records, as the listings give them: another
   investigator's noise is not listed for an ordinary user.
*/
bool US_DataProcess::foreign_noise_of_edit( int editID,
                                            QList< ForeignNoise >& noise )
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

   // noiseID, noiseGUID, editedDataID, modelID, noiseType, modelGUID,
   // timeEntered, checksum, size, description
   while ( status == IUS_DB2::OK  &&  db->next() )
   {
      if ( db->value( 2 ).toInt() != editID )             continue;
      if ( models.contains( db->value( 3 ).toInt() ) )    continue;

      ForeignNoise fn;
      fn.noiseID     = db->value( 0 ).toInt();
      fn.guid        = db->value( 1 ).toString();
      fn.modelID     = db->value( 3 ).toInt();
      fn.description = db->value( 9 ).toString();
      noise << fn;
   }

   return true;
}

/* Remove an edit, and the foreign noise recorded against it.

   The edit is removed in a transaction, and the database is asked first as
   it is.  A patched delete_editedData removes all noise recorded against the
   edit with it, and changes nothing when it fails (it uses a savepoint in
   the caller's transaction); unless the caller is an administrator it
   refuses (EDIT_KEPT) when that would take along records of another
   investigator or records the GMP framework refers to, and that refusal is
   not tried again.  An older one
   removes the edit's models and their noise, and only then fails on noise
   of another edit's model still recorded against the edit
   (fk_noise_editDataID): the rollback gives the models and noise back.
   Only then, and only when that noise is the investigator's own, is it
   removed first in a second transaction, and the edit asked for again.
   Whenever a removal fails, or its outcome is not confirmed, what the
   database holds is read back (settle_edit).
*/
int US_DataProcess::remove_edit( int row )
{
   int         editID = cdesc.recordID;
   QString     label  = cdesc.label;
   QList< ForeignNoise > foreign;

   if ( ! foreign_noise_of_edit( editID, foreign ) )
   {
      errMsg = tr( "%1 was not removed: %2" ).arg( label ).arg( errMsg );
      return 2017;
   }

   QString why;
   bool    clean = true;           // a failure was rolled back as a whole
   int     stat  = edit_attempt( editID, QList< ForeignNoise >(), why, clean );

   // Only an older database's refusal on the noise of other edits' models
   // recorded against the edit (fk_noise_editDataID) can change when the
   // investigator's own such noise goes first.  Any other is not tried
   // again: the edit is not theirs, the database keeps it for records of
   // another investigator or of the GMP framework, it is gone, another
   // session holds what it needs (a lock wait or a deadlock, which a second
   // try would only wait on again), or the connection is lost.
   if ( stat == 2012  &&  clean  &&  edit_failure == EDIT_FK  &&
        ! foreign.isEmpty() )
   {  // An older database: the noise of other edits' models goes first
      why.clear();
      stat = edit_attempt( editID, foreign, why, clean );
   }

   if ( stat == 0 )
   {  // Gone, with its models and their noise, and no noise recorded
      // against it can be left
      QVector< int > drows = da_model->descendant_rows( row );

      for ( int ii = 0; ii < drows.size(); ii++ )
         if ( ( da_model->row_datadesc( drows[ ii ] ).recState
                & US_DataModel::REC_DB ) != 0 )
            mark_removed_from_db( drows[ ii ] );

      for ( int ii = 0; ii < foreign.size(); ii++ )
         mark_noise_removed( foreign.at( ii ).noiseID );

      return 0;
   }

   // A failure the transaction gave back as a whole changed nothing, and
   // there is nothing to read back -- unless the database said the edit is
   // not there, which is what the read back finds out
   if ( clean  &&  edit_failure != EDIT_MISSING )
   {
      errMsg = tr( "%1 was not removed: %2\n\nNothing was removed." )
               .arg( label ).arg( why );
      return stat;
   }

   return settle_edit( row, foreign, stat, why );
}

// One transaction of an edit removal: the given noise, then the edit
int US_DataProcess::edit_attempt( int editID,
                                  const QList< ForeignNoise >& first,
                                  QString& why, bool& clean )
{
   QStringList query;
   QString     terr;

   clean        = true;
   edit_status  = IUS_DB2::OK;
   edit_failure = EDIT_REFUSED;

   if ( ! db->beginTransaction( terr ) )
   {
      why = tr( "a database transaction could not be started (%1)." )
            .arg( terr );
      return 2019;
   }

   int stat = 0;

   for ( int ii = 0; ii < first.size(); ii++ )
   {
      const ForeignNoise& fn = first.at( ii );
      query.clear();
      query << "delete_noise" << QString::number( fn.noiseID );

      int nst = db->statusQuery( query );

      if ( nst != IUS_DB2::OK )
      {
         why  = tr( "noise %1 of another edit's model was computed from it,"
                    " the database keeps the edit while that noise refers to"
                    " it, and the noise could not be removed (delete_noise"
                    " status %2: %3)" )
                .arg( fn.description.isEmpty() ? fn.guid : fn.description )
                .arg( nst ).arg( db->lastError() );
         stat = 2018;
         edit_failure = failure_kind( nst, db->lastServerErrno(),
                                      db->lastError() );
         why += failure_text( edit_failure );
         break;
      }
   }

   if ( stat == 0 )
   {
      query.clear();
      query << "delete_editedData" << QString::number( editID );
DbgLv(1) << "EDT_RMV:" << query;

      int     est  = db->statusQuery( query );
      int     serr = db->lastServerErrno();
      QString derr = db->lastError();
      edit_status  = est;

      if ( est != IUS_DB2::OK )
      {
DbgLv(1) << "EDT_RMV:   stat" << est << derr << serr;
         edit_failure = failure_kind( est, serr, derr );

         if ( edit_failure == EDIT_LOST )
            why  = tr( "the connection to the database was lost while it"
                       " was being removed (delete_editedData: %1), so"
                       " whether it was removed is not known." ).arg( derr );

         else if ( edit_failure == EDIT_DEADLOCK  ||
                   edit_failure == EDIT_TIMEOUT )
            why  = tr( "the database gave up on it (delete_editedData status"
                       " %1: %2)." ).arg( est ).arg( derr );

         else
            why  = tr( "the database refused it (delete_editedData status"
                       " %1: %2)." ).arg( est ).arg( derr );

         why += failure_text( edit_failure );
         stat = 2012;
      }
   }

   if ( stat == 0 )
   {
      if ( db->commitTransaction( terr ) )
         return 0;

      // Not confirmed: still open as far as the client knows
      QString rerr;
      db->rollbackTransaction( rerr );
      why   = tr( "the removal could not be committed: %1" ).arg( terr );
      clean = false;
      return 2019;
   }

   // A deadlock ends the transaction on the server, which rolls back all of
   // it: the rollback here is refused for that, and nothing is left done
   if ( ! db->rollbackTransaction( terr )  &&  edit_failure != EDIT_DEADLOCK )
   {
      why  += tr( "\nThe database transaction could not be rolled back as a"
                  " whole: %1" ).arg( terr );
      clean = false;
   }

   if ( edit_failure == EDIT_LOST )
      clean = false;                // what the server did is not known

   return stat;
}

// What kind of failure a delete_editedData or delete_noise status says
US_DataProcess::EditFailure US_DataProcess::failure_kind(
      int status, int serverErrno, const QString& error )
{
   // A patched delete_editedData catches what fails inside it and reports
   // it in its status and message ("... error 1205: ..."), or raises it
   // again; an older one has the server report it
   auto says = [ & ]( int code )
   {
      return ( serverErrno == code  ||
               error.contains( QString( "error %1:" ).arg( code ) ) );
   };

   if ( US_DataModel::connection_lost( serverErrno ) )
   {
      conn_lost = true;
      return EDIT_LOST;
   }

   if ( says( 1213 ) )                                     return EDIT_DEADLOCK;
   if ( says( 1205 ) )                                     return EDIT_TIMEOUT;
   if ( status == EDIT_KEPT )                              return EDIT_KEPT_BY_DB;
   if ( status == IUS_DB2::NO_EDITDATA  ||
        status == IUS_DB2::NO_EXPERIMENT )                 return EDIT_MISSING;
   if ( serverErrno == 1451  ||
        status == IUS_DB2::CONSTRAINT_FAILED )             return EDIT_FK;

   return EDIT_REFUSED;
}

// What a kind of failure means for the user
QString US_DataProcess::failure_text( EditFailure kind )
{
   switch ( kind )
   {
      case EDIT_KEPT_BY_DB:
         return tr( "\nThe database keeps the edit when it is not yours,"
                    " or when removing it would take along records of"
                    " another investigator -- noise computed from it for"
                    " that investigator's model of another edit, or that"
                    " investigator's models of it -- or records the GMP"
                    " framework refers to; its message says which.  Only"
                    " that investigator (or an administrator) can remove"
                    " those records, and nothing was changed." );

      case EDIT_FK:
         return tr( "\nAnother record still refers to the edit.  On an"
                    " older database that may be noise computed from it"
                    " for a model of another investigator, which this"
                    " window cannot list; only that investigator (or an"
                    " administrator) can remove that noise." );

      case EDIT_DEADLOCK:
         return tr( "\nAnother session was changing the same records at"
                    " the same time (a deadlock), and the database rolled"
                    " back the whole removal.  Try again." );

      case EDIT_TIMEOUT:
         return tr( "\nAnother session holds records the removal needs,"
                    " and the database waited for them too long (a lock"
                    " wait timeout).  Try again once that work is done." );

      case EDIT_MISSING:
         return tr( "\nThe database has no such edit, or no experiment for"
                    " it: another session may have removed it." );

      default:
         return QString();
   }
}

// What to do when what the database holds could not be read back
QString US_DataProcess::read_back_advice( void ) const
{
   if ( conn_lost )
      return lost_text();

   return tr( "Scan again before acting on these records." );
}

// What to do once the connection to the database is lost
QString US_DataProcess::lost_text( void ) const
{
   return tr( "The connection to the database was lost, and this window"
              " does not connect again: close it and open it again, and"
              " look at these records before acting on them." );
}

// Say what to do when an action failed on a lost connection: every action
// after it fails too, since the window does not connect again
int US_DataProcess::note_lost( int stat )
{
   if ( stat == 0  ||  stat == IN_SYNC  ||  db == NULL )   return stat;

   if ( US_DataModel::connection_lost( db->lastServerErrno() ) )
      conn_lost = true;

   if ( conn_lost  &&  ! errMsg.contains( lost_text() ) )
      errMsg += "\n\n" + lost_text();

   return stat;
}

// Whether a record is still in the database
int US_DataProcess::db_has( const QString& proc, int id, int gone )
{
   QStringList query;
   query << proc << QString::number( id );
   db->query( query );

   int status = db->lastErrno();

   if ( status == IUS_DB2::OK )
      return db->next() ? 1 : 0;

   if ( status == gone  ||  status == IUS_DB2::NOROWS )
      return 0;

   if ( US_DataModel::connection_lost( db->lastServerErrno() ) )
      conn_lost = true;

   return -1;
}

// Mark as gone every row of a noise record, wherever it is in the tree
void US_DataProcess::mark_noise_removed( int noiseID )
{
   // Also when the tree does not show it yet: its experiment may be read
   // later, from what the catalog read before it went
   da_model->removed_from_db( US_DataModel::NOISE, noiseID );

   for ( int nrow = 0; nrow < da_model->recCount(); nrow++ )
   {
      US_DataModel::DataDesc ndesc = da_model->row_datadesc( nrow );

      if ( ndesc.recType == US_DataModel::NOISE  &&
           ndesc.recordID == noiseID  &&
           ( ndesc.recState & US_DataModel::REC_DB ) != 0 )
         mark_removed_from_db( nrow );
   }
}

/* What the database holds after an edit removal that failed, or whose
   outcome is not known.

   An older delete_editedData has removed the edit's models and noise
   before it fails, and a transaction that a newer one committed has kept
   the foreign noise removed before it: whatever the transaction did not
   give back is read here, so no row shows a record the database no longer
   holds.
*/
int US_DataProcess::settle_edit( int row, const QList< ForeignNoise >& foreign,
                                 int stat, const QString& why )
{
   US_DataModel::DataDesc edesc = da_model->row_datadesc( row );
   QStringList gone;
   bool        unknown = false;

   int edit = db_has( "get_editedData", edesc.recordID, IUS_DB2::NO_EDITDATA );

   // The edit's own models and noise, as the tree shows them
   QVector< int > drows = da_model->descendant_rows( row );

   for ( int ii = 0; ii < drows.size(); ii++ )
   {
      US_DataModel::DataDesc ddesc = da_model->row_datadesc( drows[ ii ] );

      if ( ( ddesc.recState & US_DataModel::REC_DB ) == 0  ||
           ddesc.recordID < 0 )
         continue;

      int has = ( ddesc.recType == US_DataModel::MODEL )
              ? db_has( "get_model_info", ddesc.recordID, IUS_DB2::NO_MODEL )
              : db_has( "get_noise_info", ddesc.recordID, IUS_DB2::NO_NOISE );

      if ( has < 0 )
         unknown = true;

      else if ( has == 0 )
      {
         gone << ddesc.label;
         mark_removed_from_db( drows[ ii ] );
      }
   }

   // The noise of other edits' models taken on with it
   for ( int ii = 0; ii < foreign.size(); ii++ )
   {
      const ForeignNoise& fn = foreign.at( ii );
      int has = db_has( "get_noise_info", fn.noiseID, IUS_DB2::NO_NOISE );

      if ( has < 0 )
         unknown = true;

      else if ( has == 0 )
      {
         gone << ( fn.description.isEmpty() ? fn.guid : fn.description );
         mark_noise_removed( fn.noiseID );
      }
   }

   if ( edit == 0 )
      return 0;                     // removed after all

   errMsg = tr( "%1 was not removed: %2" ).arg( edesc.label ).arg( why );

   if ( edit < 0 )
   {
      unknown = true;
      stat    = ( stat == 0 ) ? 2019 : stat;
   }

   if ( ! gone.isEmpty() )
      errMsg += tr( "\n\nThese records were removed from the database all"
                    " the same, and their rows say so:\n  %1" )
                .arg( gone.join( "\n  " ) );

   else if ( ! unknown )
      errMsg += tr( "\n\nNothing was removed." );

   if ( unknown )
      errMsg += tr( "\n\nWhat the database holds now could not all be read"
                    " back (%1).  %2" )
                .arg( db->lastError() ).arg( read_back_advice() );

   return stat;
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

   conn_lost = conn_lost  ||
               US_DataModel::connection_lost( db->lastServerErrno() );
   errMsg = tr( "%1 was written to the database, but its database ID could"
                " not be read (%2: %3).  %4" )
            .arg( cdesc.label ).arg( proc ).arg( db->lastError() )
            .arg( conn_lost ? read_back_advice()
                            : tr( "Scan again before acting on it." ) );
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

// Replace the last error message
void US_DataProcess::setError( const QString& message )
{
   errMsg = message;
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
   bool    xml = ( desc.recType == US_DataModel::MODEL  ||
                   desc.recType == US_DataModel::NOISE );

   if ( ( desc.recState & US_DataModel::HV_DET ) != 0 )
   {  // Compared already: the database copy is the one compared then
      dbd = desc.contents.section( " ", 0, 1 ).simplified();

      // A model or noise comparison may have been of the bytes as they are
      // (the catalogs' checksums) or of the XML with LF line ends (one
      // record at a time): a file whose bytes are what the database holds
      // is the same either way; any other is compared again, the database
      // copy as the file is, with LF line ends
      if ( xml  &&  ! dbd.isEmpty()  &&
           dbd == US_DataModel::file_digest( desc.filename ) )
         return 1;

      if ( xml )
      {
         QString error;

         if ( ! US_DataModel::db_digest( db, desc, dbd, error ) )
            dbd.clear();
      }
   }

   else
   {  // Compare them now, this record only
      QString error;

      if ( ! US_DataModel::db_digest( db, desc, dbd, error ) )
         dbd.clear();
   }

   // The file may have changed since the comparison: it is what would be
   // uploaded, so it is what is hashed
   lod = US_DataModel::local_digest( desc );

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

