//! \file datapub_noise_rules.h
//! \brief Which model's noise the analysis programs load for an edit, as
//!        US_LoadableNoise::count_noise() decides it, for the import tests
#ifndef DATAPUB_NOISE_RULES_H
#define DATAPUB_NOISE_RULES_H

#include <QDir>
#include <QFile>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include "ius_db2.h"

/* The analysis programs (2DSA, GA, PCSA, fematch, ...) load the noise of an
   edit without a model through US_LoadableNoise::count_noise()
   (gui/us_loadable_noise.cpp): of the models of the edit that have noise,
   the latest -- the first row of get_model_desc_by_editID, which lists the
   highest modelID first, in a database; the last M*.xml file in name order
   on disk -- and its noise is loaded by default.  A Monte Carlo request
   counts as one model: the first of its files on disk, the last of its
   rows (the lowest ID) in a database.  These functions give that
   model's GUID the same way, for a test to compare a target with its
   source; test/gui/test_us_loadable_noise_gui.cpp holds them to
   count_noise() itself on disk.
*/
class DataPubNoiseRules
{
   public:
      //! The latest model with noise of an edit in a disk store's data
      //! directory, as count_noise() picks it; empty for none
      static QString latestModelOnDisk( const QString& dataDir,
                                        const QString& editGUID )
      {
         QSet< QString > withNoise;
         QDir            ndir( dataDir + "/noises" );

         for ( const QString& name : ndir.entryList( QStringList( "N*.xml" ),
                                                     QDir::Files, QDir::Name ) )
         {
            QMap< QString, QString > attrs =
                  first( ndir.filePath( name ), "noise" );
            withNoise << attrs.value( "modelGUID" );
         }

         QDir        mdir( dataDir + "/models" );
         QStringList requests;
         QString     latest;

         for ( const QString& name : mdir.entryList( QStringList( "M*.xml" ),
                                                     QDir::Files, QDir::Name ) )
         {
            QMap< QString, QString > attrs =
                  first( mdir.filePath( name ), "model" );
            QString guid = attrs.value( "modelGUID" );
            QString mc   = attrs.value( "monteCarlo" );

            if ( ! withNoise.contains( guid )  ||
                 attrs.value( "editGUID" ) != editGUID )
               continue;

            if ( ! mc.isEmpty()  &&  mc != "0" )
            {
               if ( requests.contains( attrs.value( "requestGUID" ) ) )
                  continue;

               requests << attrs.value( "requestGUID" );
            }

            latest = guid;
         }

         return latest;
      }

      //! The latest model with noise of an edit in a database, as
      //! count_noise() picks it (get_noise_desc_by_editID and
      //! get_model_desc_by_editID of the investigator); empty for none
      static QString latestModelInDb( IUS_DB2* db, int invID,
                                      const QString& editGUID )
      {
         QStringList query;
         query << "get_editID" << editGUID;
         db->query( query );

         if ( ! db->next() )  return QString();

         QString editID = db->value( 0 ).toString();
         QSet< QString > withNoise;

         query.clear();
         query << "get_noise_desc_by_editID" << QString::number( invID )
               << editID;
         db->query( query );

         while ( db->next() )
            withNoise << db->value( 5 ).toString();

         query.clear();
         query << "get_model_desc_by_editID" << QString::number( invID )
               << editID;
         db->query( query );

         QStringList models;
         QStringList descs;

         while ( db->next() )
         {
            QString guid = db->value( 1 ).toString();

            if ( withNoise.contains( guid )  &&
                 db->value( 6 ).toString() == editID )
            {
               models << guid;
               descs  << db->value( 2 ).toString();
            }
         }

         // One model of a Monte Carlo request counts: going from the back,
         // as count_noise() does, the last listed stays
         QStringList requests;

         for ( int ii = models.size() - 1; ii >= 0; ii-- )
         {
            QString asys = descs[ ii ].section( ".", -2, -2 );

            if ( ! ( asys.contains( "-MC" )  &&  asys.contains( "_mc" ) ) )
               continue;

            QString request = asys.section( "_", 0, -2 );

            if ( requests.contains( request ) )
               models.removeAt( ii );
            else
               requests << request;
         }

         return models.isEmpty() ? QString() : models.first();
      }

   private:
      static QMap< QString, QString > first( const QString& path,
                                             const QString& element )
      {
         QMap< QString, QString > attrs;
         QFile                    file( path );

         if ( ! file.open( QIODevice::ReadOnly | QIODevice::Text ) )
            return attrs;

         QXmlStreamReader xml( &file );

         while ( ! xml.atEnd() )
         {
            xml.readNext();

            if ( xml.isStartElement()  &&  xml.name() == element )
            {
               for ( const QXmlStreamAttribute& attr : xml.attributes() )
                  attrs.insert( attr.name().toString(),
                                attr.value().toString() );
               break;
            }
         }

         return attrs;
      }
};

#endif
