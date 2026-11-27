//! \file datapub_craft.h
//! \brief A bundle with its manifest changed, packed again, for the import
//!        tests: what another exporter -- or an older one -- would write
#ifndef DATAPUB_CRAFT_H
#define DATAPUB_CRAFT_H

#include <functional>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

#include "us_datapub_bundle.h"
#include "us_datapub_export.h"
#include "us_datapub_hash.h"
#include "us_datapub_import.h"
#include "us_datapub_manifest.h"

/* The bundle \a from unpacked, its manifest changed by \a change (which
   also gets the staging directory, for a payload to change), given a
   matching manifest digest and packed as \a to.  Empty on failure, with
   \a error saying why. */
inline QString datapubRecraft(
      const QString& from, const QString& to,
      std::function< void( US_DataPubManifest&, const QString& ) > change,
      QString& error )
{
   US_DataPubImporter source;

   if ( ! source.inspect( from, error ) )  return QString();

   US_DataPubBundle out;

   if ( ! out.createStaging( error ) )  return QString();

   QString      stage = out.stagingPath();
   QDir         src( source.bundleRoot() );
   QDirIterator it( source.bundleRoot(), QDir::Files,
                    QDirIterator::Subdirectories );

   while ( it.hasNext() )
   {
      QString rel = src.relativeFilePath( it.next() );
      QDir().mkpath( QFileInfo( stage + "/" + rel ).path() );

      if ( ! QFile::copy( source.bundleRoot() + "/" + rel,
                          stage + "/" + rel ) )
      {
         error = "cannot copy " + rel;
         return QString();
      }
   }

   US_DataPubManifest mani;
   QString            mpath = stage + "/" + US_DataPubBundle::manifestName();

   if ( ! mani.read( mpath, error ) )  return QString();

   change( mani, stage );

   if ( ! mani.write( mpath, error ) )  return QString();

   QFile digest( stage + "/" + US_DataPubExporter::manifestDigestName() );

   if ( ! digest.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
   {
      error = "cannot write the manifest digest";
      return QString();
   }

   digest.write( ( US_DataPubHash::fileHash( mpath ) + "  "
                   + US_DataPubBundle::manifestName() + "\n" ).toUtf8() );
   digest.close();

   QFile::remove( to );

   if ( ! out.pack( to, error ) )  return QString();

   return to;
}

/* A bundle of a disk export made to look like a database export: the
   source "db", the models and noise records listed newest first, as the
   catalog lists a database's.  \a keepOrder keeps the source's time stamps
   and file numbers (sourceLastUpdated, sourceIndex); without it they are
   taken out, as in a bundle of an export before they were written, and
   \a ids gives each record its file number as the database ID instead (an
   older database export's modelID / noiseID) -- or nothing, which leaves
   the manifest's order the only one there is. */
inline void datapubAsDatabaseExport( US_DataPubManifest& mani, bool keepOrder,
                                     bool ids )
{
   mani.source = "db";

   for ( US_DataPub::EntityType type : { US_DataPub::Model,
                                         US_DataPub::Noise } )
   {
      QList< US_DataPubEntity > list = mani.section( type );
      QList< US_DataPubEntity > reversed;

      for ( int ii = list.size() - 1; ii >= 0; ii-- )
      {
         US_DataPubEntity entity = list[ ii ];
         QString          number = entity.filename.mid( 1, 7 );

         entity.filename.clear();
         entity.id = ( ! keepOrder  &&  ids )
                     ? QString::number( number.toInt() ) : QString( "-1" );

         if ( ! keepOrder )
         {
            entity.attrs.remove( "sourceLastUpdated" );
            entity.attrs.remove( "sourceIndex" );
         }

         reversed << entity;
      }

      mani.setSection( type, reversed );
   }
}

#endif
