//! \file gf_basis.cpp
//! \brief Exports the ASTFEM basis (A matrix) of a solute grid for a dataset
//!        written by gf_testdata, and runs the real US_SolveSim global fit on
//!        given data, for the NNLS-level global-fit study (nnls_study.py).
//!
//! Usage:
//!   gf_basis basis <dir> <runID> <out-prefix>
//!       writes <out-prefix>.A.bin (float64, column-major, rows = scans x
//!       points scan-major, one column per grid solute in sorted order) and
//!       <out-prefix>.json (grid, dimensions, ODlimit)
//!   gf_basis fit <dir> <runID,runID,...> <b.bin,b.bin,...> <odlim,odlim,...>
//!       <out.json>
//!       global calc_residuals over the grid with the given data (float64,
//!       scan-major) and OD limits; writes the concentrations and variances
//!   gf_basis sim <dir> <runID> <model.xml> <out-prefix>
//!       simulates a model file (20,W) for the dataset; writes
//!       <out-prefix>.data.bin and <out-prefix>.sim.bin (float64, scan-major)
//!       and <out-prefix>.json (radii, scan times, ODlimit)
//!
//! The dataset setup follows us_mpi_analysis (initFromData, rotor stretch,
//! edited bottom, generated timestate, s/D corrections).

#include <QtCore>
#include <algorithm>
#include <cstdio>

#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_model.h"
#include "us_dataIO.h"
#include "us_math2.h"
#include "us_solute.h"
#include "us_solve_sim.h"

namespace
{
const double kDensity   = 0.998234;
const double kViscosity = 1.001940;
const double kVbar20    = 0.72;
const double kCpBottom  = 7.20;

// Grid:  s 1-10 S by 0.5 S, f/f0 1-3 by 0.25, plus the true species
QVector<US_Solute> grid_solutes( void )
{
   QVector<US_Solute> sols;
   for ( int is = 0; is <= 18; is++ )
   {
      for ( int ik = 0; ik <= 8; ik++ )
      {
         sols << US_Solute( ( 1.0 + 0.5 * is ) * 1.0e-13, 1.0 + 0.25 * ik, 0.0, kVbar20 );
      }
   }
   const double ss[3]  = { 2.5e-13, 4.5e-13, 7.0e-13 };
   const double ff0[3] = { 1.25, 1.6, 2.1 };
   for ( int ii = 0; ii < 3; ii++ )
   {
      US_Solute sol( ss[ii], ff0[ii], 0.0, kVbar20 );
      if ( !sols.contains( sol ) )
      {
         sols << sol;
      }
   }
   std::sort( sols.begin(), sols.end() );
   return sols;
}

// Optional overrides for other data:  GF_EDIT (edit file name, %1 = runID),
//  GF_DENSITY, GF_VISCOSITY, GF_VBAR
double env_value( const char* name, double dflt )
{
   const QByteArray val = qgetenv( name );
   return val.isEmpty() ? dflt : val.toDouble();
}

bool load_dataset( const QString& dir, const QString& runID, US_SolveSim::DataSet& dset, QTemporaryDir& tmpdir )
{
   const double density   = env_value( "GF_DENSITY", kDensity );
   const double viscosity = env_value( "GF_VISCOSITY", kViscosity );
   const double vbar20    = env_value( "GF_VBAR", kVbar20 );
   QString editname = qEnvironmentVariableIsSet( "GF_EDIT" )
                         ? QString( qgetenv( "GF_EDIT" ) ).arg( runID )
                         : runID + ".e1.RI.1.A.260.xml";
   if ( US_DataIO::loadData( dir, editname, dset.run_data ) != US_DataIO::OK )
   {
      fprintf( stderr, "cannot load %s/%s\n", qPrintable( dir ), qPrintable( editname ) );
      return false;
   }
   US_DataIO::EditedData&   edata     = dset.run_data;
   US_SimulationParameters& simparams = dset.simparams;
   simparams.initFromData( NULL, edata, true );
   simparams.rotorcoeffs[0]  = 0.0;
   simparams.rotorcoeffs[1]  = 1.0e-12;
   simparams.bottom_position = kCpBottom;
   simparams.bottom          = edata.bottom;
   simparams.band_forming    = false;

   QString            tmst = tmpdir.path() + "/" + runID + ".time_state.tmst";
   US_DataIO::RawData tsdat;
   US_AstfemMath::initSimData( tsdat, edata, 0.0 );
   US_AstfemMath::writetimestate( tmst, simparams, tsdat );
   simparams.simSpeedsFromTimeState( tmst );
   dset.tmst_file = tmst;

   US_Math2::SolutionData sd;
   sd.density   = density;
   sd.viscosity = viscosity;
   sd.manual    = false;
   sd.vbar20    = vbar20;
   sd.vbar      = vbar20;
   US_Math2::data_correction( 20.0, sd );

   dset.viscosity          = viscosity;
   dset.density            = density;
   dset.compress           = 0.0;
   dset.temperature        = 20.0;
   dset.vbar20             = vbar20;
   dset.vbartb             = vbar20;
   dset.s20w_correction    = sd.s20w_correction;
   dset.D20w_correction    = sd.D20w_correction;
   dset.rotor_stretch[0]   = 0.0;
   dset.rotor_stretch[1]   = 1.0e-12;
   dset.centerpiece_bottom = kCpBottom;
   for ( double& zc : dset.zcoeffs )
   {
      zc = 0.0;
   }
   dset.solute_type = 0;
   dset.manual      = false;
   return true;
}

US_SolveSim::Simulation new_sim( const QVector<US_Solute>& sols )
{
   US_SolveSim::Simulation sim;
   sim.solutes  = sols;
   sim.noisflag = 0;
   sim.alpha    = 0.0;
   sim.maxrss   = 0;
   return sim;
}

int do_basis( const QString& dir, const QString& runID, const QString& prefix )
{
   QTemporaryDir        tmpdir;
   US_SolveSim::DataSet dset;
   if ( !load_dataset( dir, runID, dset, tmpdir ) )
   {
      return 1;
   }
   QList<US_SolveSim::DataSet*> dsets;
   dsets << &dset;

   // The A matrix only depends on the geometry:  disable the OD limit
   dset.run_data.ODlimit      = 1.0e+99;
   QVector<US_Solute> sols    = grid_solutes();
   US_SolveSim::Simulation sim = new_sim( sols );
   QVector<double> amat;
   QVector<double> bvec;
   US_SolveSim solvesim( dsets, 1, false );
   solvesim.calc_residuals( 0, 1, sim, false, &amat, &bvec );

   int nrows = dset.run_data.scanCount() * dset.run_data.pointCount();
   if ( amat.size() != nrows * sols.size() )
   {
      fprintf( stderr, "A size %lld != %d x %lld (columns cut?)\n", (long long) amat.size(), nrows, (long long) sols.size() );
      return 1;
   }
   QFile fa( prefix + ".A.bin" );
   fa.open( QIODevice::WriteOnly );
   fa.write( (const char*) amat.data(), amat.size() * sizeof( double ) );
   fa.close();

   QJsonObject meta;
   QJsonArray  js, jk;
   for ( const US_Solute& sol : sols )
   {
      js << sol.s * 1.0e13;
      jk << sol.k;
   }
   meta["s"]       = js;
   meta["ff0"]     = jk;
   meta["nscans"]  = dset.run_data.scanCount();
   meta["npoints"] = dset.run_data.pointCount();
   meta["rpm"]     = dset.run_data.scanData[0].rpm;
   QFile fj( prefix + ".json" );
   fj.open( QIODevice::WriteOnly );
   fj.write( QJsonDocument( meta ).toJson() );
   fj.close();
   printf( "%s: %d rows x %lld columns\n", qPrintable( prefix ), nrows, (long long) sols.size() );
   return 0;
}

int do_fit( const QString& dir, const QStringList& runs, const QStringList& bfiles, const QStringList& odlims, const QString& out )
{
   QTemporaryDir                     tmpdir;
   QList<US_SolveSim::DataSet*>      dsets;
   QVector<US_SolveSim::DataSet*>    owned;
   for ( int ee = 0; ee < runs.size(); ee++ )
   {
      US_SolveSim::DataSet* dset = new US_SolveSim::DataSet;
      owned << dset;
      if ( !load_dataset( dir, runs[ee], *dset, tmpdir ) )
      {
         return 1;
      }
      QFile fb( bfiles[ee] );
      fb.open( QIODevice::ReadOnly );
      QByteArray      raw = fb.readAll();
      const double*   bv  = (const double*) raw.constData();
      int             kk  = 0;
      int             nsc = dset->run_data.scanCount();
      int             npt = dset->run_data.pointCount();
      if ( raw.size() != (qsizetype) ( nsc * npt * sizeof( double ) ) )
      {
         fprintf( stderr, "b file %s has wrong size\n", qPrintable( bfiles[ee] ) );
         return 1;
      }
      for ( int ss = 0; ss < nsc; ss++ )
      {
         for ( int rr = 0; rr < npt; rr++ )
         {
            dset->run_data.setValue( ss, rr, bv[kk++] );
         }
      }
      dset->run_data.ODlimit = odlims[ee].toDouble();
      dsets << dset;
   }

   QVector<US_Solute>      sols = grid_solutes();
   US_SolveSim::Simulation sim  = new_sim( sols );
   US_SolveSim             solvesim( dsets, 1, false );
   solvesim.calc_residuals( 0, dsets.size(), sim );

   QJsonObject res;
   QJsonArray  jsol, jvar;
   for ( const US_Solute& sol : sim.solutes )
   {
      QJsonArray one;
      one << sol.s * 1.0e13 << sol.k << sol.c;
      jsol << one;
   }
   for ( double var : sim.variances )
   {
      jvar << var;
   }
   res["solutes"]   = jsol;
   res["variances"] = jvar;
   res["variance"]  = sim.variance;
   QFile fj( out );
   fj.open( QIODevice::WriteOnly );
   fj.write( QJsonDocument( res ).toJson() );
   fj.close();
   qDeleteAll( owned );
   return 0;
}
int do_sim( const QString& dir, const QString& runID, const QString& mfile, const QString& prefix )
{
   QTemporaryDir        tmpdir;
   US_SolveSim::DataSet dset;
   if ( !load_dataset( dir, runID, dset, tmpdir ) )
   {
      return 1;
   }
   US_Model model;
   if ( model.load( mfile ) != 0 )
   {
      fprintf( stderr, "cannot load model %s\n", qPrintable( mfile ) );
      return 1;
   }
   model.update_coefficients();
   // 20,W to experimental space, as US_SolveSim does
   for ( int ii = 0; ii < model.components.size(); ii++ )
   {
      model.components[ii].s /= dset.s20w_correction;
      model.components[ii].D /= dset.D20w_correction;
   }
   US_DataIO::EditedData& edata = dset.run_data;
   US_DataIO::RawData     simdat;
   US_AstfemMath::initSimData( simdat, edata, 0.0 );
   US_Astfem_RSA astfem( model, dset.simparams );
   astfem.calculate( simdat );

   int             nsc = edata.scanCount();
   int             npt = edata.pointCount();
   QVector<double> dv, sv;
   for ( int ss = 0; ss < nsc; ss++ )
   {
      for ( int rr = 0; rr < npt; rr++ )
      {
         dv << edata.value( ss, rr );
         sv << simdat.value( ss, rr );
      }
   }
   QFile fd( prefix + ".data.bin" );
   fd.open( QIODevice::WriteOnly );
   fd.write( (const char*) dv.data(), dv.size() * sizeof( double ) );
   fd.close();
   QFile fs( prefix + ".sim.bin" );
   fs.open( QIODevice::WriteOnly );
   fs.write( (const char*) sv.data(), sv.size() * sizeof( double ) );
   fs.close();

   QJsonObject meta;
   QJsonArray  jr, jt;
   for ( int rr = 0; rr < npt; rr++ )
   {
      jr << edata.radius( rr );
   }
   for ( int ss = 0; ss < nsc; ss++ )
   {
      jt << edata.scanData[ss].seconds;
   }
   meta["radius"]  = jr;
   meta["seconds"] = jt;
   meta["ODlimit"] = edata.ODlimit;
   meta["nscans"]  = nsc;
   meta["npoints"] = npt;
   QFile fj( prefix + ".json" );
   fj.open( QIODevice::WriteOnly );
   fj.write( QJsonDocument( meta ).toJson() );
   fj.close();
   return 0;
}
} // namespace

int main( int argc, char* argv[] )
{
   QCoreApplication app( argc, argv );
   QStringList      args = app.arguments();

   if ( args.size() == 5 && args[1] == "basis" )
   {
      return do_basis( args[2], args[3], args[4] );
   }
   if ( args.size() == 7 && args[1] == "fit" )
   {
      return do_fit( args[2], args[3].split( "," ), args[4].split( "," ), args[5].split( "," ), args[6] );
   }
   if ( args.size() == 6 && args[1] == "sim" )
   {
      return do_sim( args[2], args[3], args[4], args[5] );
   }
   fprintf( stderr,
            "usage: %s basis <dir> <runID> <out-prefix>\n"
            "       %s fit <dir> <runIDs> <bfiles> <odlims> <out.json>\n"
            "       %s sim <dir> <runID> <model.xml> <out-prefix>\n",
            argv[0],
            argv[0],
            argv[0] );
   return 2;
}
