//! \file datapub_borrow_scenario.h
//! \brief An edit with several noise records of a type and models that
//!        borrow them, for the import's borrowing tests
#ifndef DATAPUB_BORROW_SCENARIO_H
#define DATAPUB_BORROW_SCENARIO_H

#include <QDateTime>
#include <QFile>
#include <QMap>
#include <QString>

#include "us_datapub_manifest.h"
#include "us_model.h"
#include "us_noise.h"
#include "us_util.h"

/* Models and noise of one edit, dated (file times) in this order, ten
   seconds apart:

     m0  a model with no noise, made before any noise: borrows none
     a   the fixture's model, with its ti noise n1
     b   a model with the ri noise r
     c   a model with no noise: borrows n1 (ti) and r (ri)
     d   a model with the ti noise n2, newer than c
     e   a model with no noise: borrows n2 (ti) and r (ri)

   Written in the bundle's order, or all in one second, a target makes m0
   borrow n1 and r, and c borrow n2 (or e n1): the catalog has only the
   time stamps to go by.
*/
class DataPubBorrowScenario
{
   public:
      QString m0, b, c, d, e;      // model GUIDs
      QString n1, r, n2;           // noise GUIDs

      //! \param models  The models directory of the source
      //! \param noises  The noise directory of the source
      //! \param runID   The run the models belong to
      //! \param editGUID The edit
      //! \param n1A     The fixture's ti noise (N0000001.xml), fitted to
      //!                its model (M0000001.xml)
      void write( const QString& models, const QString& noises,
                  const QString& runID, const QString& editGUID,
                  const QString& n1A )
      {
         n1 = n1A;
         m0 = US_Util::new_guid();
         b  = US_Util::new_guid();
         c  = US_Util::new_guid();
         d  = US_Util::new_guid();
         e  = US_Util::new_guid();
         r  = US_Util::new_guid();
         n2 = US_Util::new_guid();

         QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs( -3600 );

         model( models + "/M0000010.xml", m0, editGUID,
                runID + ".1A280.e1_a1_GA_local.model", t0 );
         stamp( models + "/M0000001.xml", t0.addSecs( 10 ) );
         stamp( noises + "/N0000001.xml", t0.addSecs( 10 ) );
         model( models + "/M0000011.xml", b, editGUID,
                runID + ".1A280.e1_a2_2DSA_local_ri.model", t0.addSecs( 20 ) );
         noise( noises + "/N0000011.xml", r, b, US_Noise::RI,
                runID + ".1A280.e1_a2_2DSA_local_ri.ri_noise", t0.addSecs( 20 ) );
         model( models + "/M0000012.xml", c, editGUID,
                runID + ".1A280.e1_a3_GA_local.model", t0.addSecs( 30 ) );
         model( models + "/M0000013.xml", d, editGUID,
                runID + ".1A280.e1_a4_2DSA_local_ti.model", t0.addSecs( 40 ) );
         noise( noises + "/N0000012.xml", n2, d, US_Noise::TI,
                runID + ".1A280.e1_a4_2DSA_local_ti.ti_noise", t0.addSecs( 40 ) );
         model( models + "/M0000014.xml", e, editGUID,
                runID + ".1A280.e1_a5_GA_local.model", t0.addSecs( 50 ) );
      }

      //! The latest model with noise of the edit, whose noise the analysis
      //! programs load (US_LoadableNoise::count_noise()): d, the last file
      QString latest( void ) const { return d; }

      //! What each noise record of the edit is borrowed by, as the source
      //! has it
      QMap< QString, QString > expected( void ) const
      {
         QStringList both = QStringList() << c << e;
         std::sort( both.begin(), both.end() );

         QMap< QString, QString > map;
         map.insert( n1, c );
         map.insert( r,  both.join( " " ) );
         map.insert( n2, e );
         return map;
      }

      //! noise GUID -> borrowedByModelGUIDs, of every noise of a manifest
      static QMap< QString, QString > borrowers(
            const US_DataPubManifest& manifest )
      {
         QMap< QString, QString >  map;
         QList< US_DataPubEntity > noises =
               manifest.section( US_DataPub::Noise );

         for ( int ii = 0; ii < noises.size(); ii++ )
            map.insert( noises[ ii ].guid,
                        noises[ ii ].attrs.value( "borrowedByModelGUIDs" ) );

         return map;
      }

      //! A map of borrowers as text a failed comparison shows
      static std::string text( const QMap< QString, QString >& map )
      {
         QStringList lines;

         for ( auto it = map.constBegin(); it != map.constEnd(); ++it )
            lines << it.key() + " <- [" + it.value() + "]";

         return lines.join( "\n" ).toStdString();
      }

   protected:
      static void stamp( const QString& path, const QDateTime& when )
      {
         QFile file( path );

         if ( file.open( QIODevice::ReadWrite ) )
         {
            file.setFileTime( when, QFileDevice::FileModificationTime );
            file.close();
         }
      }

      static void model( const QString& path, const QString& guid,
                         const QString& editGUID, const QString& desc,
                         const QDateTime& when )
      {
         US_Model model;
         model.modelGUID   = guid;
         model.editGUID    = editGUID;
         model.description = desc;
         model.write( path );
         stamp( path, when );
      }

      static void noise( const QString& path, const QString& guid,
                         const QString& modelGUID, US_Noise::NoiseType type,
                         const QString& desc, const QDateTime& when )
      {
         US_Noise noise;
         noise.noiseGUID   = guid;
         noise.modelGUID   = modelGUID;
         noise.description = desc;
         noise.type        = type;
         noise.minradius   = 5.8;
         noise.maxradius   = 5.95;

         for ( int ii = 0; ii < 16; ii++ )
            noise.values << 0.002 * double( ii );

         noise.count = noise.values.size();
         noise.write( path );
         stamp( path, when );
      }
};

/* Another edit's models and noise, which an import that picked noise
   records greedily (the order of a database export's manifest, newest
   first) could not write in any order: dated ten seconds apart,

     a   the fixture's model, with its ti noise n0
     x0  a model with no noise: borrows n0
     b   a model with the ti noise n1
     x1  a model with no noise: borrows n1
     c   a model with the ri noise n2
     x2  a model with no noise: borrows n1 and n2
*/
class DataPubDeadEndScenario : public DataPubBorrowScenario
{
   public:
      QString x0, x1, x2, bb, cc;  // model GUIDs
      QString n0, n1, n2;          // noise GUIDs

      void write( const QString& models, const QString& noises,
                  const QString& runID, const QString& editGUID,
                  const QString& n0A )
      {
         n0 = n0A;
         x0 = US_Util::new_guid();
         x1 = US_Util::new_guid();
         x2 = US_Util::new_guid();
         bb = US_Util::new_guid();
         cc = US_Util::new_guid();
         n1 = US_Util::new_guid();
         n2 = US_Util::new_guid();

         QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs( -3600 );

         stamp( models + "/M0000001.xml", t0 );
         stamp( noises + "/N0000001.xml", t0 );
         model( models + "/M0000010.xml", x0, editGUID,
                runID + ".1A280.e1_x0_GA_local.model", t0.addSecs( 10 ) );
         model( models + "/M0000011.xml", bb, editGUID,
                runID + ".1A280.e1_b_2DSA_local_ti.model", t0.addSecs( 20 ) );
         noise( noises + "/N0000011.xml", n1, bb, US_Noise::TI,
                runID + ".1A280.e1_b_2DSA_local_ti.ti_noise", t0.addSecs( 20 ) );
         model( models + "/M0000012.xml", x1, editGUID,
                runID + ".1A280.e1_x1_GA_local.model", t0.addSecs( 30 ) );
         model( models + "/M0000013.xml", cc, editGUID,
                runID + ".1A280.e1_c_2DSA_local_ri.model", t0.addSecs( 40 ) );
         noise( noises + "/N0000012.xml", n2, cc, US_Noise::RI,
                runID + ".1A280.e1_c_2DSA_local_ri.ri_noise", t0.addSecs( 40 ) );
         model( models + "/M0000014.xml", x2, editGUID,
                runID + ".1A280.e1_x2_GA_local.model", t0.addSecs( 50 ) );
      }

      QString latest( void ) const { return cc; }

      QMap< QString, QString > expected( void ) const
      {
         QStringList both = QStringList() << x1 << x2;
         std::sort( both.begin(), both.end() );

         QMap< QString, QString > map;
         map.insert( n0, x0 );
         map.insert( n1, both.join( " " ) );
         map.insert( n2, x2 );
         return map;
      }
};

#endif
