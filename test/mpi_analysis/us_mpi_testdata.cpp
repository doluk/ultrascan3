//! \file us_mpi_testdata.cpp
//! \brief Writes synthetic sedimentation velocity datasets for the
//!        us_mpi_analysis regression tests.
//!
//! The data are simulated with the same simulation-parameter setup that
//! us_mpi_analysis uses for a dataset (initFromData, rotor stretch, bottom,
//! generated 1-second timestate), so the analysis can fit them closely.
//! Noise comes from a portable generator so the files are identical on every
//! platform.
//!
//! Usage:
//!   us_mpi_testdata data   <outdir> <runID> <cell> <seed> <scale>
//!   us_mpi_testdata cgrid  <outdir> <filename>
//!
//! "data" writes <runID>.RI.<cell>.A.260.auc and the edit file
//! <runID>.e1.RI.<cell>.A.260.xml.  "cgrid" writes a custom-grid model file.

#include <QtCore>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_dataIO.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_util.h"

namespace
{
// Experiment and sample constants shared with the job files written by
// us_mpi_regression.py.  Keep both in sync.
const double kRpm        = 60000.0;
const double kAccelRate  = 400.0;   // rpm per second
const double kMeniscus   = 5.90;
const double kCpBottom   = 7.20;
const double kStretch0   = 0.0;
const double kStretch1   = 1.0e-12;
const double kDensity    = 0.998234;
const double kViscosity  = 1.001940;
const double kVbar20     = 0.72;
const double kNoiseSigma = 0.004;
const int    kScans      = 30;
const double kRadiusLo   = 5.95;
const double kRadiusHi   = 7.15;
const double kRadiusStep = 0.006;

// Deterministic, platform-independent normal deviates (xorshift64* plus
// Box-Muller), unlike std::normal_distribution.
class Rng
{
public:
   explicit Rng( uint64_t seed ) : state( seed ? seed : 0x9E3779B97F4A7C15ULL ) {}

   double uniform()
   {
      state ^= state >> 12;
      state ^= state << 25;
      state ^= state >> 27;
      uint64_t xx = state * 2685821657736338717ULL;
      return ( (double)( xx >> 11 ) + 0.5 ) / 9007199254740992.0;
   }

   double normal()
   {
      double u1 = uniform();
      double u2 = uniform();
      return sqrt( -2.0 * log( u1 ) ) * cos( 2.0 * M_PI * u2 );
   }

private:
   uint64_t state;
};

QString fmt( double value )
{
   return QString::number( value, 'g', 12 );
}

US_DataIO::RawData skeleton( const QString& runID, int cell, uint64_t seed )
{
   US_DataIO::RawData raw;
   memcpy( raw.type, "RI", 2 );
   raw.cell        = cell;
   raw.channel     = 'A';
   raw.description = "us_mpi_analysis regression data " + runID;

   // GUID bytes derived from seed and cell so every dataset differs
   Rng guid( seed * 131 + cell );
   for ( int ii = 0; ii < 16; ii++ )
      raw.rawGUID[ ii ] = (char)( guid.uniform() * 256.0 );

   int npoints = qRound( ( kRadiusHi - kRadiusLo ) / kRadiusStep ) + 1;
   for ( int rr = 0; rr < npoints; rr++ )
      raw.xvalues << kRadiusLo + rr * kRadiusStep;

   // Linear acceleration to kRpm: omega^2 t integrates to w^2 (t - 2/3 t_acc)
   double omega = kRpm * M_PI / 30.0;
   double t_acc = kRpm / kAccelRate;
   for ( int ss = 0; ss < kScans; ss++ )
   {
      US_DataIO::Scan scan;
      scan.temperature = 20.0;
      scan.rpm         = kRpm;
      scan.seconds     = 600.0 + 240.0 * ss;
      scan.omega2t     = omega * omega * ( scan.seconds - 2.0 * t_acc / 3.0 );
      scan.wavelength  = 260.0;
      scan.plateau     = 0.0;
      scan.delta_r     = kRadiusStep;
      scan.nz_stddev   = false;
      scan.rvalues.fill( 0.0, npoints );
      scan.interpolated = QByteArray( ( npoints + 7 ) / 8, '\0' );
      raw.scanData << scan;
   }

   return raw;
}

bool write_edit( const QString& path, const QString& runID,
                 const US_DataIO::RawData& raw, int cell )
{
   QFile file( path );
   if ( ! file.open( QIODevice::WriteOnly | QIODevice::Text ) )
      return false;

   QXmlStreamWriter xml( &file );
   xml.setAutoFormatting( true );
   xml.writeStartDocument();
   xml.writeDTD( "<!DOCTYPE UltraScanEdits>" );
   xml.writeStartElement( "experiment" );
   xml.writeAttribute( "type", "Velocity" );

   xml.writeStartElement( "identification" );
   xml.writeStartElement( "runid" );
   xml.writeAttribute( "value", runID );
   xml.writeEndElement();
   xml.writeStartElement( "editGUID" );
   xml.writeAttribute( "value", "00000000-0000-4000-8000-0000000000"
                       + QString::number( 10 + cell ) );
   xml.writeEndElement();
   xml.writeStartElement( "rawDataGUID" );
   xml.writeAttribute( "value", US_Util::uuid_unparse( (uchar*)raw.rawGUID ) );
   xml.writeEndElement();
   xml.writeEndElement();  // identification

   xml.writeStartElement( "run" );
   xml.writeAttribute( "cell",       QString::number( cell ) );
   xml.writeAttribute( "channel",    "A" );
   xml.writeAttribute( "wavelength", "260" );

   xml.writeStartElement( "parameters" );
   xml.writeStartElement( "meniscus" );
   xml.writeAttribute( "radius", fmt( kMeniscus ) );
   xml.writeEndElement();
   xml.writeStartElement( "data_range" );
   xml.writeAttribute( "left",  "6.0" );
   xml.writeAttribute( "right", "7.1" );
   xml.writeEndElement();
   xml.writeStartElement( "plateau" );
   xml.writeAttribute( "radius", "7.0" );
   xml.writeEndElement();
   xml.writeStartElement( "baseline" );
   xml.writeAttribute( "radius", "7.12" );
   xml.writeEndElement();
   xml.writeStartElement( "od_limit" );
   xml.writeAttribute( "value", "1.5" );
   xml.writeEndElement();
   xml.writeEndElement();  // parameters

   xml.writeEndElement();  // run
   xml.writeEndElement();  // experiment
   xml.writeEndDocument();
   return true;
}

US_Model sample_model( double scale )
{
   // Three species spanning the 2DSA grid used by the regression jobs
   const double ss [ 3 ] = { 2.5e-13, 4.5e-13, 7.0e-13 };
   const double ff0[ 3 ] = { 1.25,    1.6,     2.1     };
   const double cc [ 3 ] = { 0.25,    0.40,    0.30    };

   US_Model model;
   for ( int ii = 0; ii < 3; ii++ )
   {
      US_Model::SimulationComponent comp;
      comp.s                    = ss [ ii ];
      comp.f_f0                 = ff0[ ii ];
      comp.vbar20               = kVbar20;
      comp.signal_concentration = cc [ ii ] * scale;
      model.components << comp;
   }
   model.update_coefficients();
   return model;
}

int write_data( const QString& outdir, const QString& runID, int cell,
                uint64_t seed, double scale )
{
   QString triple   = QString( "RI.%1.A.260" ).arg( cell );
   QString aucname  = runID + "." + triple + ".auc";
   QString editname = runID + ".e1." + triple + ".xml";

   US_DataIO::RawData raw = skeleton( runID, cell, seed );
   if ( US_DataIO::writeRawData( outdir + "/" + aucname, raw ) != US_DataIO::OK
     || ! write_edit( outdir + "/" + editname, runID, raw, cell ) )
   {
      fprintf( stderr, "cannot write %s\n", qPrintable( aucname ) );
      return 1;
   }

   // Load through the same path us_mpi_analysis uses
   US_DataIO::EditedData edata;
   if ( US_DataIO::loadData( outdir, editname, edata ) != US_DataIO::OK )
   {
      fprintf( stderr, "cannot load %s\n", qPrintable( editname ) );
      return 1;
   }

   US_SimulationParameters simparams;
   simparams.initFromData( NULL, edata, true );
   double stretch[ 2 ]          = { kStretch0, kStretch1 };
   simparams.rotorcoeffs[ 0 ]   = stretch[ 0 ];
   simparams.rotorcoeffs[ 1 ]   = stretch[ 1 ];
   simparams.bottom_position    = kCpBottom;
   simparams.bottom             = kCpBottom;
   simparams.band_forming       = false;

   QTemporaryDir tmpdir;
   QString tmst = tmpdir.path() + "/sim.time_state.tmst";
   US_DataIO::RawData tsdat;
   US_AstfemMath::initSimData( tsdat, edata, 0.0 );
   US_AstfemMath::writetimestate( tmst, simparams, tsdat );
   simparams.simSpeedsFromTimeState( tmst );

   // Convert the 20,W model to experimental space as US_SolveSim does
   US_Math2::SolutionData sd;
   sd.density   = kDensity;
   sd.viscosity = kViscosity;
   sd.manual    = false;
   sd.vbar20    = kVbar20;
   sd.vbar      = kVbar20;
   US_Math2::data_correction( 20.0, sd );

   US_Model model = sample_model( scale );
   for ( int ii = 0; ii < model.components.size(); ii++ )
   {
      model.components[ ii ].s /= sd.s20w_correction;
      model.components[ ii ].D /= sd.D20w_correction;
   }

   US_DataIO::RawData simdat;
   US_AstfemMath::initSimData( simdat, edata, 0.0 );
   US_Astfem_RSA astfem( model, simparams );
   astfem.calculate( simdat );

   // Copy simulation plus noise into the raw radial grid
   int    offset = US_DataIO::index( raw.xvalues, edata.xvalues[ 0 ] );
   Rng    noise( seed );
   for ( int ss = 0; ss < raw.scanCount(); ss++ )
   {
      for ( int rr = 0; rr < raw.pointCount(); rr++ )
      {
         int    er    = rr - offset;
         double value = ( er >= 0  &&  er < simdat.pointCount() )
                        ? simdat.value( ss, er ) : 0.0;
         raw.scanData[ ss ].rvalues[ rr ] = value + kNoiseSigma * noise.normal();
      }
   }

   if ( US_DataIO::writeRawData( outdir + "/" + aucname, raw ) != US_DataIO::OK )
   {
      fprintf( stderr, "cannot rewrite %s\n", qPrintable( aucname ) );
      return 1;
   }

   double vmax = 0.0;
   for ( int rr = 0; rr < simdat.pointCount(); rr++ )
      vmax = qMax( vmax, simdat.value( 0, rr ) );
   printf( "%s %s  first-scan max %.4f  last-scan mid %.4f\n",
           qPrintable( aucname ), qPrintable( editname ), vmax,
           simdat.value( simdat.scanCount() - 1, simdat.pointCount() / 2 ) );
   return 0;
}

int write_custom_grid( const QString& outdir, const QString& filename )
{
   // A small s/vbar custom grid with fixed f/f0, split into 4 subgrids
   US_Model model;
   model.description = "regression custom grid";
   model.modelGUID   = "00000000-0000-4000-8000-00000000c9d0";
   model.analysis    = US_Model::CUSTOMGRID;
   model.subGrids    = 4;

   for ( int ii = 0; ii < 24; ii++ )
   {
      for ( int jj = 0; jj < 5; jj++ )
      {
         US_Model::SimulationComponent comp;
         comp.s                    = ( 1.5 + ii * 0.3 ) * 1.0e-13;
         comp.vbar20               = 0.70 + jj * 0.01;
         comp.f_f0                 = 1.6;
         comp.signal_concentration = 0.0;
         model.components << comp;
      }
   }
   model.update_coefficients();

   if ( model.write( outdir + "/" + filename ) != 0 )
   {
      fprintf( stderr, "cannot write %s\n", qPrintable( filename ) );
      return 1;
   }
   printf( "%s\n", qPrintable( filename ) );
   return 0;
}
}  // namespace

int main( int argc, char* argv[] )
{
   QCoreApplication app( argc, argv );
   QStringList args = app.arguments();

   if ( args.size() == 7  &&  args[ 1 ] == "data" )
      return write_data( args[ 2 ], args[ 3 ], args[ 4 ].toInt(),
                         args[ 5 ].toULongLong(), args[ 6 ].toDouble() );

   if ( args.size() == 4  &&  args[ 1 ] == "cgrid" )
      return write_custom_grid( args[ 2 ], args[ 3 ] );

   fprintf( stderr,
            "usage: %s data <outdir> <runID> <cell> <seed> <scale>\n"
            "       %s cgrid <outdir> <filename>\n", argv[ 0 ], argv[ 0 ] );
   return 2;
}
