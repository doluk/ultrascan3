//! \file us_3dsa_cli.cpp
#include "us_3dsa_cli.h"

#include "us_astfem_math.h"
#include "us_buffer.h"
#include "us_dataIO.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_solute.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>

namespace {

// ---------------------------------------------------------------------------
// D2O volume percent -> buffer density and viscosity at 20 C.
//
// Linear interpolation between pure H2O and pure D2O.  Good enough for a test
// harness -- what the fit sees is the density the case declares, and the same
// number is handed to both the simulator and the fitter, so any departure from
// real D2O mixtures cancels.  Real work should use measured values.
// ---------------------------------------------------------------------------
const double H2O_DENSITY   = 0.998234;
const double H2O_VISCOSITY = 1.001940;
const double D2O_DENSITY   = 1.105000;
const double D2O_VISCOSITY = 1.251000;

double d2o_density( double pct )
{
   return H2O_DENSITY + ( D2O_DENSITY - H2O_DENSITY ) * pct / 100.0;
}

double d2o_viscosity( double pct )
{
   return H2O_VISCOSITY + ( D2O_VISCOSITY - H2O_VISCOSITY ) * pct / 100.0;
}

// Geometry shared by every generated case.  Small enough to keep the harness
// runnable in minutes, large enough to resolve a boundary.
const double SIM_MENISCUS = 5.85;
const double SIM_BOTTOM   = 7.15;
const double SIM_DELTA_R  = 0.005;
const int    SIM_SCANS    = 12;
const int    SIM_RPM      = 45000;
const int    SIM_POINTS   = 100;
const double SIM_TEMP     = 20.0;
const int    SIM_HOURS    = 5;

QTextStream& out()
{
   static QTextStream ts( stdout );
   return ts;
}

QTextStream& err()
{
   static QTextStream ts( stderr );
   return ts;
}

double jnum( const QJsonObject& o, const QString& key, double dflt )
{
   return o.contains( key ) ? o.value( key ).toDouble() : dflt;
}

int jint( const QJsonObject& o, const QString& key, int dflt )
{
   return o.contains( key ) ? o.value( key ).toInt() : dflt;
}

bool jbool( const QJsonObject& o, const QString& key, bool dflt )
{
   return o.contains( key ) ? o.value( key ).toBool() : dflt;
}

US_Math2::SolutionData corrections( double vbar20, double density,
                                    double viscosity, double temperature )
{
   US_Math2::SolutionData sd;
   sd.density   = density;
   sd.viscosity = viscosity;
   sd.manual    = false;
   sd.vbar20    = vbar20;
   sd.vbar      = US_Math2::adjust_vbar20( vbar20, temperature );
   US_Math2::data_correction( temperature, sd );
   return sd;
}

} // namespace

// ---------------------------------------------------------------------------
void US_3dsaCli::usage()
{
   out()
      << "us_3dsa_cli -- headless 3DSA fit driver\n\n"
      << "  us_3dsa_cli fit --config <case.json> [--out <result.json>]\n"
      << "                  [--model <model.xml>] [--threads N] [--quiet]\n\n"
      << "      Fit the buoyancy-contrast series the case file describes and\n"
      << "      report the global RMSD and the RMSD of every data set.\n\n"
      << "  us_3dsa_cli gen-cases --outdir <dir>\n\n"
      << "      Write the test-case tree: one JSON case file per case, the\n"
      << "      model / buffer / simulation-parameter XML each data set needs,\n"
      << "      and manifest.json naming the us_astfem_sim runs that produce\n"
      << "      the data.\n\n"
      << "  us_3dsa_cli --help\n";
   out().flush();
}

// ---------------------------------------------------------------------------
double US_3dsaCli::weighted( const US_Model& m, int attr )
{
   double num = 0.0;
   double den = 0.0;

   for ( int cc = 0; cc < m.components.size(); cc++ )
   {
      const US_Model::SimulationComponent& c = m.components[ cc ];
      const double w = c.signal_concentration;

      double v = 0.0;
      switch ( attr )
      {
         case US_Solute::ATTR_S:  v = c.s;      break;
         case US_Solute::ATTR_K:  v = c.f_f0;   break;
         case US_Solute::ATTR_V:  v = c.vbar20; break;
         case US_Solute::ATTR_D:  v = c.D;      break;
         default:                 v = c.mw;     break;
      }

      num += v * w;
      den += w;
   }

   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

double US_3dsaCli::truth_vbar( const Case& kase )
{
   double num = 0.0, den = 0.0;
   for ( const Species& s : kase.species ) { num += s.vbar20 * s.conc;
                                             den += s.conc; }
   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

double US_3dsaCli::truth_s( const Case& kase )
{
   double num = 0.0, den = 0.0;
   for ( const Species& s : kase.species ) { num += s.s20w * s.conc;
                                             den += s.conc; }
   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

// ---------------------------------------------------------------------------
bool US_3dsaCli::read_case( const QString& path, Case& kase, QString& emsg )
{
   QFile f( path );

   if ( ! f.open( QIODevice::ReadOnly | QIODevice::Text ) )
   {
      emsg = QString( "cannot open %1" ).arg( path );
      return false;
   }

   QJsonParseError perr;
   const QJsonDocument doc = QJsonDocument::fromJson( f.readAll(), &perr );
   f.close();

   if ( doc.isNull() || ! doc.isObject() )
   {
      emsg = QString( "%1: %2" ).arg( path ).arg( perr.errorString() );
      return false;
   }

   const QJsonObject root = doc.object();
   kase.name        = root.value( "name"        ).toString();
   kase.description = root.value( "description" ).toString();

   for ( const QJsonValue& v : root.value( "species" ).toArray() )
   {
      const QJsonObject o = v.toObject();
      Species sp;
      sp.s20w   = jnum( o, "s20w",   0.0 );
      sp.ff0    = jnum( o, "ff0",    1.0 );
      sp.vbar20 = jnum( o, "vbar20", 0.72 );
      sp.conc   = jnum( o, "conc",   1.0 );
      kase.species << sp;
   }

   for ( const QJsonValue& v : root.value( "datasets" ).toArray() )
   {
      const QJsonObject o = v.toObject();
      DataSetSpec ds;
      ds.auc         = o.value( "auc"       ).toString();
      ds.simparams   = o.value( "simparams" ).toString();
      ds.label       = o.value( "label"     ).toString();
      ds.d2o_percent = jnum( o, "d2o_percent", 0.0 );
      ds.density     = jnum( o, "density",     H2O_DENSITY );
      ds.viscosity   = jnum( o, "viscosity",   H2O_VISCOSITY );
      ds.temperature = jnum( o, "temperature", SIM_TEMP );
      ds.loading     = jnum( o, "loading",     1.0 );
      ds.vbar20      = jnum( o, "vbar20",      0.72 );
      kase.datasets << ds;
   }

   const QJsonObject noi = root.value( "noise" ).toObject();
   kase.rnoise  = jnum( noi, "random", 0.0 );
   kase.tinoise = jnum( noi, "ti",     0.0 );
   kase.rinoise = jnum( noi, "ri",     0.0 );

   const QJsonObject g = root.value( "grid" ).toObject();
   kase.s_min     = jnum( g, "s_min", kase.s_min );
   kase.s_max     = jnum( g, "s_max", kase.s_max );
   kase.s_res     = jint( g, "s_res", kase.s_res );
   kase.k_min     = jnum( g, "k_min", kase.k_min );
   kase.k_max     = jnum( g, "k_max", kase.k_max );
   kase.k_res     = jint( g, "k_res", kase.k_res );
   kase.v_min     = jnum( g, "v_min", kase.v_min );
   kase.v_max     = jnum( g, "v_max", kase.v_max );
   kase.v_res     = jint( g, "v_res", kase.v_res );
   kase.grid_reps = jint( g, "grid_reps", kase.grid_reps );

   const QJsonObject fo = root.value( "fit" ).toObject();
   kase.threads         = jint ( fo, "threads",         kase.threads );
   kase.noisflag        = jint ( fo, "noisflag",        kase.noisflag );
   kase.fit_scales      = jbool( fo, "fit_scales",      kase.fit_scales );
   kase.edit_margin     = jnum ( fo, "edit_margin",     kase.edit_margin );
   kase.ignore_contrast = jbool( fo, "ignore_contrast", kase.ignore_contrast );

   const QJsonObject ex = root.value( "expect" ).toObject();
   kase.vbar_tol    = jnum ( ex, "vbar_tol",    kase.vbar_tol );
   kase.s_tol       = jnum ( ex, "s_tol",       kase.s_tol );
   kase.must_refuse = jbool( ex, "must_refuse", kase.must_refuse );
   kase.rmsd_max        = jnum( ex, "rmsd_max",        kase.rmsd_max );
   kase.rmsd_spread_max = jnum( ex, "rmsd_spread_max", kase.rmsd_spread_max );

   if ( kase.datasets.isEmpty() )
   {
      emsg = QString( "%1: no datasets" ).arg( path );
      return false;
   }

   return true;
}

// ---------------------------------------------------------------------------
bool US_3dsaCli::load_datasets( const Case& kase,
                                QList< US_SolveSim::DataSet* >& dsets,
                                QString& emsg )
{
   const double edit_margin = kase.edit_margin;

   for ( int ii = 0; ii < kase.datasets.size(); ii++ )
   {
      const DataSetSpec& spec = kase.datasets[ ii ];

      US_DataIO::RawData raw;
      const int rc = US_DataIO::readRawData( spec.auc, raw );

      if ( rc != US_DataIO::OK )
      {
         emsg = QString( "cannot read %1 (code %2)" ).arg( spec.auc ).arg( rc );
         return false;
      }

      if ( raw.scanCount() < 1  ||  raw.pointCount() < 1 )
      {
         emsg = QString( "%1 holds no scans" ).arg( spec.auc );
         return false;
      }

      US_SolveSim::DataSet* d = new US_SolveSim::DataSet;

      // Simulation parameters: the same file the data was generated from, so
      // the fit's forward model matches the simulator exactly.
      if ( ! spec.simparams.isEmpty() )
      {
         if ( d->simparams.load_simparms( spec.simparams ) != 0 )
         {
            emsg = QString( "cannot read %1" ).arg( spec.simparams );
            delete d;
            return false;
         }
      }

      d->run_data.runID      = QFileInfo( spec.auc ).completeBaseName();
      d->run_data.dataType   = "RA";
      d->run_data.cell       = QString::number( raw.cell );
      d->run_data.channel    = QString( QChar( raw.channel ) );
      d->run_data.wavelength =
         QString::number( qRound( raw.scanData[ 0 ].wavelength ) );
      d->run_data.expType    = "velocity";
      d->run_data.meniscus   = d->simparams.meniscus;
      d->run_data.bottom     = d->simparams.bottom;
      d->run_data.baseline   = 0.0;
      d->run_data.plateau    = 0.0;
      d->run_data.ODlimit    = 1.0e+30;

      // Trim to an edited radial range, as us_edit would.  The simulator
      // writes the full cell, meniscus to bottom inclusive; the fit
      // interpolates its own simulation onto these radii and refuses a range
      // that reaches the ends exactly.  Real edited data never does.
      const double r_lo = d->simparams.meniscus + edit_margin;
      const double r_hi = d->simparams.bottom   - edit_margin;

      QVector< int > keep;
      keep.reserve( raw.xvalues.size() );

      for ( int rr = 0; rr < raw.xvalues.size(); rr++ )
      {
         if ( raw.xvalues[ rr ] >= r_lo  &&  raw.xvalues[ rr ] <= r_hi )
            keep << rr;
      }

      if ( keep.size() < 10 )
      {
         emsg = QString( "%1: only %2 points survive the %3 cm edit margin" )
                .arg( spec.auc ).arg( keep.size() ).arg( edit_margin );
         delete d;
         return false;
      }

      d->run_data.xvalues.clear();
      d->run_data.xvalues.reserve( keep.size() );
      for ( int rr : keep )  d->run_data.xvalues << raw.xvalues[ rr ];

      d->run_data.scanData.clear();
      d->run_data.scanData.reserve( raw.scanData.size() );

      for ( int ss = 0; ss < raw.scanData.size(); ss++ )
      {
         US_DataIO::Scan sc = raw.scanData[ ss ];
         sc.rvalues.clear();
         sc.rvalues.reserve( keep.size() );
         for ( int rr : keep )  sc.rvalues << raw.scanData[ ss ].rvalues[ rr ];
         d->run_data.scanData << sc;
      }

      US_SimulationParameters::computeSpeedSteps( &d->run_data.scanData,
                                                  d->simparams.speed_step );

      d->density     = spec.density;
      d->viscosity   = spec.viscosity;
      d->temperature = spec.temperature;
      d->manual      = false;
      d->compress    = 0.0;
      d->vbar20      = spec.vbar20;

      US_Math2::SolutionData sd = corrections( spec.vbar20, spec.density,
                                               spec.viscosity,
                                               spec.temperature );
      d->vbartb            = sd.vbar;
      d->s20w_correction   = sd.s20w_correction;
      d->D20w_correction   = sd.D20w_correction;
      d->centerpiece_bottom = d->simparams.bottom;

      dsets << d;
   }

   return true;
}

// ---------------------------------------------------------------------------
int US_3dsaCli::run_fit( const QStringList& args )
{
   QString cfg_path;
   QString out_path;
   QString model_path;
   int     threads = -1;
   bool    quiet   = false;

   for ( int ii = 0; ii < args.size(); ii++ )
   {
      const QString a = args[ ii ];

      if      ( a == "--config"  &&  ii + 1 < args.size() ) cfg_path   = args[ ++ii ];
      else if ( a == "--out"     &&  ii + 1 < args.size() ) out_path   = args[ ++ii ];
      else if ( a == "--model"   &&  ii + 1 < args.size() ) model_path = args[ ++ii ];
      else if ( a == "--threads" &&  ii + 1 < args.size() ) threads    = args[ ++ii ].toInt();
      else if ( a == "--quiet" )                            quiet      = true;
      else
      {
         err() << "unknown fit option: " << a << "\n";
         err().flush();
         return 2;
      }
   }

   if ( cfg_path.isEmpty() )
   {
      err() << "fit needs --config <case.json>\n";
      err().flush();
      return 2;
   }

   Case    kase;
   QString emsg;

   if ( ! read_case( cfg_path, kase, emsg ) )
   {
      err() << "case error: " << emsg << "\n";
      err().flush();
      return 2;
   }

   if ( threads > 0 )  kase.threads = threads;

   QList< US_SolveSim::DataSet* > dsets;

   if ( ! load_datasets( kase, dsets, emsg ) )
   {
      err() << "data error: " << emsg << "\n";
      err().flush();
      qDeleteAll( dsets );
      return 3;
   }

   US_3dsaProcess::Parameters p;
   p.x_min = kase.s_min;  p.x_max = kase.s_max;  p.x_res = kase.s_res;
   p.y_min = kase.k_min;  p.y_max = kase.k_max;  p.y_res = kase.k_res;
   p.z_min = kase.v_min;  p.z_max = kase.v_max;  p.z_res = kase.v_res;
   p.grid_reps       = kase.grid_reps;
   p.s_mask          = US_3dsaProcess::mask_s_k_v();
   p.nthreads        = kase.threads;
   p.noisflag        = kase.noisflag;
   p.fit_scales      = kase.fit_scales;
   p.ignore_contrast = kase.ignore_contrast;

   US_3dsaProcess proc( dsets );
   US_3dsaProcess::Result result;

   if ( ! quiet )
   {
      QObject::connect( &proc, &US_3dsaProcess::message_update,
                        [ ]( const QString& m )
                        { out() << "  | " << m << "\n"; out().flush(); } );
   }

   out() << "=== " << kase.name << " ===\n";
   if ( ! kase.description.isEmpty() )
      out() << "  " << kase.description << "\n";
   out() << QString( "  %1 data sets, %2 species, grid %3 x %4 x %5,"
                     " reps %6\n" )
            .arg( kase.datasets.size() ).arg( kase.species.size() )
            .arg( kase.s_res ).arg( kase.k_res ).arg( kase.v_res )
            .arg( kase.grid_reps );
   out().flush();

   const bool ok = proc.fit( p, result );

   QJsonObject jr;
   jr[ "case" ]     = kase.name;
   jr[ "accepted" ] = ok;

   if ( ! ok )
   {
      out() << "  REFUSED: " << proc.lastError() << "\n";
      jr[ "error" ] = proc.lastError();

      const bool as_expected = kase.must_refuse;
      out() << ( as_expected ? "  PASS (refusal expected)\n"
                             : "  FAIL (refusal not expected)\n" );
      jr[ "pass" ] = as_expected;

      if ( ! out_path.isEmpty() )
      {
         QFile f( out_path );
         if ( f.open( QIODevice::WriteOnly | QIODevice::Text ) )
         {
            f.write( QJsonDocument( jr ).toJson() );
            f.close();
         }
      }

      out().flush();
      qDeleteAll( dsets );
      return as_expected ? 0 : 1;
   }

   if ( kase.must_refuse )
   {
      out() << "  FAIL: the fit was expected to be refused but ran\n";
      jr[ "pass" ] = false;
   }

   // ---- report ------------------------------------------------------------
   const double got_vbar = weighted( result.model, US_Solute::ATTR_V );
   const double got_s    = weighted( result.model, US_Solute::ATTR_S );
   const double exp_vbar = truth_vbar( kase );
   const double exp_s    = truth_s   ( kase );

   out() << "\n";
   out() << QString( "  components %1   depth levels %2   NNLS solves %3"
                     "   Lamm solves %4   %5 s\n" )
            .arg( result.model.components.size() ).arg( result.ndepths )
            .arg( result.ntasks ).arg( result.nsimul )
            .arg( result.msecs / 1000.0, 0, 'f', 1 );
   out() << QString( "  contrast %1 (mL/g)^-1, implied vbar resolution"
                     " %2 mL/g, %3 amplitude iterations\n" )
            .arg( result.contrast, 0, 'f', 3 )
            .arg( result.vbar_resol, 0, 'f', 4 )
            .arg( result.nscaliter );

   out() << "\n  GLOBAL RMSD  " << QString::number( result.rmsd, 'e', 6 )
         << "\n\n";

   out() << QString( "  %1 %2 %3 %4 %5 %6\n" )
            .arg( "data set",   -22 ).arg( "D2O pct",     8 )
            .arg( "density",     12 ).arg( "RMSD",       12 )
            .arg( "scale fit",   12 ).arg( "scale true", 12 );

   double rmsd_lo = 1.0e+300;
   double rmsd_hi = 0.0;

   for ( int ii = 0; ii < kase.datasets.size(); ii++ )
   {
      const DataSetSpec& ds = kase.datasets[ ii ];
      const double var  = ( ii < result.variances.size() )
                          ? result.variances[ ii ] : 0.0;
      const double rmsd = sqrt( qMax( 0.0, var ) );
      const double sc   = ( ii < result.scales.size() )
                          ? result.scales[ ii ] : 1.0;
      const double sct  = ( ! kase.datasets.isEmpty() &&
                            kase.datasets[ 0 ].loading > 0.0 )
                          ? ds.loading / kase.datasets[ 0 ].loading : 1.0;

      rmsd_lo = qMin( rmsd_lo, rmsd );
      rmsd_hi = qMax( rmsd_hi, rmsd );

      out() << QString( "  %1 %2 %3 %4 %5 %6\n" )
               .arg( ds.label.isEmpty()
                     ? QFileInfo( ds.auc ).fileName() : ds.label, -22 )
               .arg( ds.d2o_percent, 8, 'f', 1 )
               .arg( ds.density,    12, 'f', 6 )
               .arg( QString::number( rmsd, 'e', 4 ), 12 )
               .arg( sc,  12, 'f', 5 )
               .arg( sct, 12, 'f', 5 );

      QJsonObject jd;
      jd[ "label" ]       = ds.label;
      jd[ "d2o_percent" ] = ds.d2o_percent;
      jd[ "density" ]     = ds.density;
      jd[ "rmsd" ]        = rmsd;
      jd[ "variance" ]    = var;
      jd[ "scale_fit" ]   = sc;
      jd[ "scale_true" ]  = sct;
      jr[ QString( "dataset_%1" ).arg( ii ) ] = jd;
   }

   out() << "\n";
   out() << QString( "  weighted vbar   fit %1   true %2   error %3\n" )
            .arg( got_vbar, 0, 'f', 5 ).arg( exp_vbar, 0, 'f', 5 )
            .arg( got_vbar - exp_vbar, 0, 'f', 5 );
   out() << QString( "  weighted s (S)  fit %1   true %2   error %3\n" )
            .arg( got_s * 1.0e13, 0, 'f', 4 )
            .arg( exp_s * 1.0e13, 0, 'f', 4 )
            .arg( ( got_s - exp_s ) * 1.0e13, 0, 'f', 4 );

   const double spread = ( rmsd_lo > 0.0 ) ? ( rmsd_hi / rmsd_lo ) : 0.0;

   const bool vbar_ok   = ( fabs( got_vbar - exp_vbar ) <= kase.vbar_tol );
   const bool s_ok      = ( fabs( got_s    - exp_s    ) <= kase.s_tol    );
   const bool rmsd_ok   = ( kase.rmsd_max <= 0.0 ) ||
                          ( result.rmsd <= kase.rmsd_max );
   const bool spread_ok = ( kase.rmsd_spread_max <= 0.0 ) ||
                          ( spread <= kase.rmsd_spread_max );
   const bool pass      = vbar_ok && s_ok && rmsd_ok && spread_ok
                          && ! kase.must_refuse;

   out() << QString( "  per-data-set RMSD spread  %1x"
                     "  (largest / smallest)\n" )
            .arg( spread, 0, 'f', 1 );

   out() << ( pass ? "  PASS\n" : "  FAIL\n" );
   if ( ! vbar_ok )
      out() << QString( "    vbar error %1 exceeds tolerance %2\n" )
               .arg( fabs( got_vbar - exp_vbar ), 0, 'f', 5 )
               .arg( kase.vbar_tol, 0, 'f', 5 );
   if ( ! s_ok )
      out() << QString( "    s error %1 S exceeds tolerance %2 S\n" )
               .arg( fabs( got_s - exp_s ) * 1.0e13, 0, 'f', 4 )
               .arg( kase.s_tol * 1.0e13, 0, 'f', 4 );
   if ( ! rmsd_ok )
      out() << QString( "    global RMSD %1 exceeds cap %2 -- the fit did not"
                        " reach the residual this case should allow\n" )
               .arg( QString::number( result.rmsd, 'e', 4 ) )
               .arg( QString::number( kase.rmsd_max, 'e', 4 ) );
   if ( ! spread_ok )
      out() << QString( "    per-data-set RMSD spread %1x exceeds cap %2x --"
                        " the fit is treating the data sets unequally\n" )
               .arg( spread, 0, 'f', 1 )
               .arg( kase.rmsd_spread_max, 0, 'f', 1 );
   out() << "\n";
   out().flush();

   jr[ "rmsd_spread" ] = spread;

   jr[ "rmsd_global" ]  = result.rmsd;
   jr[ "vbar_fit" ]     = got_vbar;
   jr[ "vbar_true" ]    = exp_vbar;
   jr[ "s_fit" ]        = got_s;
   jr[ "s_true" ]       = exp_s;
   jr[ "contrast" ]     = result.contrast;
   jr[ "vbar_resol" ]   = result.vbar_resol;
   jr[ "components" ]   = result.model.components.size();
   jr[ "nscaliter" ]    = result.nscaliter;
   jr[ "nsimul" ]       = result.nsimul;
   jr[ "msecs" ]        = (double)result.msecs;
   jr[ "pass" ]         = pass;

   if ( ! out_path.isEmpty() )
   {
      QFile f( out_path );
      if ( f.open( QIODevice::WriteOnly | QIODevice::Text ) )
      {
         f.write( QJsonDocument( jr ).toJson() );
         f.close();
      }
   }

   if ( ! model_path.isEmpty() )
      result.model.write( model_path );

   qDeleteAll( dsets );
   return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Case generation
// ---------------------------------------------------------------------------
namespace {

struct GenSpecies { double s20w, ff0, vbar20, conc; };
struct GenDataSet { double d2o_pct, loading; };

struct GenCase
{
   QString                 name;
   QString                 description;
   QVector< GenSpecies >   species;
   QVector< GenDataSet >   datasets;
   double                  rnoise  = 0.0;
   double                  tinoise = 0.0;
   double                  rinoise = 0.0;
   int                     noisflag = 0;
   double                  vbar_tol = 0.02;
   double                  s_tol    = 0.5e-13;
   double                  rmsd_max = 1.0e-3;   // noise-free cases fit to ~1e-4
   double                  rmsd_spread_max = 20.0;
   bool                    must_refuse = false;
   bool                    ignore_contrast = false;
   int                     v_res = 11;
   double                  v_min = 0.60, v_max = 0.85;
};

// The 24 cases the harness runs.  Between them they cover every axis the
// request named: species differing in s, in D, in vbar and in combination;
// mixtures sharing a vbar while differing in s and D; unequal cell loadings;
// random, time-invariant and radially-invariant noise; and D2O series of two
// to five isotope concentrations, including two that are deliberately too
// weak to determine vbar at all.
QVector< GenCase > build_cases()
{
   QVector< GenCase > cases;

   const GenSpecies A { 4.0e-13, 1.5, 0.7300, 0.50 };   // the reference
   const GenSpecies B { 6.5e-13, 1.5, 0.7300, 0.30 };   // same vbar, larger s
   const GenSpecies C { 4.0e-13, 2.5, 0.7300, 0.30 };   // same s, smaller D
   const GenSpecies D { 4.0e-13, 1.5, 0.6900, 0.40 };   // lower vbar
   const GenSpecies E { 6.5e-13, 2.0, 0.7700, 0.40 };   // higher vbar, other s
   const GenSpecies F { 2.0e-13, 1.3, 0.7300, 0.35 };   // small, same vbar
   const GenSpecies G { 8.0e-13, 1.8, 0.7500, 0.25 };   // large, other vbar

   auto add = [ & ]( const GenCase& c ) { cases << c; };

   // ---- 1-5: one species, growing series and loading spread --------------
   { GenCase c; c.name = "case01_single_2buffers";
     c.description = "One species, H2O and 100% D2O, equal loading, no noise";
     c.species << A; c.datasets << GenDataSet{ 0.0, 1.0 }
                                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case02_single_3buffers";
     c.description = "One species, 0/50/100% D2O, equal loading, no noise";
     c.species << A; c.datasets << GenDataSet{ 0.0, 1.0 }
                                << GenDataSet{ 50.0, 1.0 }
                                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case03_single_5buffers";
     c.description = "One species, 0/25/50/75/100% D2O, equal loading";
     c.species << A;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 }
                << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case04_single_unequal_loading";
     c.description = "One species, 0/50/100% D2O, loadings 1.0 / 0.7 / 1.4";
     c.species << A; c.datasets << GenDataSet{ 0.0, 1.0 }
                                << GenDataSet{ 50.0, 0.7 }
                                << GenDataSet{ 100.0, 1.4 };
     add( c ); }

   { GenCase c; c.name = "case05_single_extreme_loading";
     c.description = "One species, loadings 1.0 / 0.4 / 2.0 -- five-fold spread";
     c.species << A; c.datasets << GenDataSet{ 0.0, 1.0 }
                                << GenDataSet{ 50.0, 0.4 }
                                << GenDataSet{ 100.0, 2.0 };
     add( c ); }

   // ---- 6-12: mixtures ---------------------------------------------------
   { GenCase c; c.name = "case06_two_species_same_vbar_diff_s";
     c.description = "Two species, same vbar, s of 4.0 and 6.5 S";
     c.species << A << B;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case07_two_species_same_vbar_diff_s_and_D";
     c.description = "Two species, same vbar, different s and different D";
     c.species << A << GenSpecies{ 6.5e-13, 2.2, 0.7300, 0.30 };
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case08_two_species_same_s_diff_D";
     c.description = "Two species, same vbar and s, f/f0 of 1.5 and 2.5";
     c.species << A << C;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     c.vbar_tol = 0.025;
     add( c ); }

   { GenCase c; c.name = "case09_two_species_diff_vbar_same_s";
     c.description = "Two species, same s, vbar of 0.73 and 0.69";
     c.species << A << D;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     c.vbar_tol = 0.025;
     add( c ); }

   { GenCase c; c.name = "case10_two_species_diff_vbar_diff_s";
     c.description = "Two species differing in s, D and vbar";
     c.species << A << E;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     c.vbar_tol = 0.03;
     add( c ); }

   { GenCase c; c.name = "case11_three_species_same_vbar";
     c.description = "Three species sharing vbar, s of 2.0, 4.0 and 6.5 S";
     c.species << F << A << B;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case12_three_species_mixed_vbar_5buffers";
     c.description = "Three species, two sharing vbar, one differing, 5 buffers";
     c.species << A << B << G;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 }
                << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     c.vbar_tol = 0.025;
     add( c ); }

   // ---- 13-19: noise -----------------------------------------------------
   { GenCase c; c.name = "case13_random_noise_low";
     c.description = "One species, random noise 0.001 OD";
     c.species << A; c.rnoise = 0.001;
     c.rmsd_max = 0.0018;   // random noise is not fitted; it stays in
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case14_random_noise_high";
     c.description = "One species, random noise 0.005 OD";
     c.species << A; c.rnoise = 0.005; c.vbar_tol = 0.02;
     c.rmsd_max = 0.0090;   // random noise is not fitted; it stays in
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case15_ti_noise";
     c.description = "One species, time-invariant noise 0.005 OD, TI fitted";
     c.species << A; c.tinoise = 0.005; c.noisflag = 1; c.vbar_tol = 0.02;
     // TI is fitted, so it must be removed: the residual should fall far
     // below the injected level, and no data set may be left out.
     c.rmsd_max = 0.0010; c.rmsd_spread_max = 5.0;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case16_ri_noise";
     c.description = "One species, radially-invariant noise 0.005 OD, RI fitted";
     c.species << A; c.rinoise = 0.005; c.noisflag = 2; c.vbar_tol = 0.02;
     c.rmsd_max = 0.0010; c.rmsd_spread_max = 5.0;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case17_all_noise";
     c.description = "One species, random + TI + RI noise, both noises fitted";
     c.species << A; c.rnoise = 0.002; c.tinoise = 0.005; c.rinoise = 0.005;
     c.noisflag = 3; c.vbar_tol = 0.02;
     // TI and RI removed, random left: the residual should sit near 0.002.
     c.rmsd_max = 0.0040; c.rmsd_spread_max = 5.0;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case18_mixture_all_noise";
     c.description = "Two species, random + TI + RI noise, both noises fitted";
     c.species << A << B;
     c.rnoise = 0.002; c.tinoise = 0.005; c.rinoise = 0.005;
     c.noisflag = 3; c.vbar_tol = 0.02;
     c.rmsd_max = 0.0040; c.rmsd_spread_max = 5.0;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case19_mixture_noise_unequal_loading_5buffers";
     c.description = "Three species, noise, unequal loadings, five isotope steps";
     c.species << F << A << B;
     c.rnoise = 0.002; c.tinoise = 0.003; c.noisflag = 1; c.vbar_tol = 0.02;
     c.rmsd_max = 0.0040; c.rmsd_spread_max = 5.0;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 0.8 }
                << GenDataSet{ 50.0, 1.3 } << GenDataSet{ 75.0, 0.6 }
                << GenDataSet{ 100.0, 1.5 };
     add( c ); }

   // ---- 20-24: edges and gates -------------------------------------------
   { GenCase c; c.name = "case20_weak_contrast_refused";
     c.description = "Only 0 and 15% D2O: contrast below the gate, must refuse";
     c.species << A; c.must_refuse = true;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 15.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case21_single_dataset_refused";
     c.description = "One data set: vbar is not determined, must refuse";
     c.species << A; c.must_refuse = true;
     c.datasets << GenDataSet{ 0.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case22_low_vbar";
     c.description = "One species at vbar 0.65, the low edge of the grid";
     c.species << GenSpecies{ 4.0e-13, 1.5, 0.6500, 0.50 };
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case23_high_vbar";
     c.description = "One species at vbar 0.80, the high edge of the grid";
     c.species << GenSpecies{ 4.0e-13, 1.5, 0.8000, 0.50 };
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     add( c ); }

   { GenCase c; c.name = "case24_wide_s_range_mixed_vbar";
     c.description = "1.5 S and 8.0 S species with different vbar, 5 buffers";
     c.species << GenSpecies{ 1.5e-13, 1.2, 0.7100, 0.40 } << G;
     c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 }
                << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                << GenDataSet{ 100.0, 1.0 };
     c.vbar_tol = 0.03;
     add( c ); }

   return cases;
}

// Write the simulation-parameter XML one data set needs.
bool write_simparams( const QString& path, const GenCase& c )
{
   US_SimulationParameters sp;
   sp.simpoints         = SIM_POINTS;
   sp.radial_resolution = SIM_DELTA_R;
   sp.meniscus          = SIM_MENISCUS;
   sp.bottom            = SIM_BOTTOM;
   sp.bottom_position   = SIM_BOTTOM;
   sp.temperature       = SIM_TEMP;
   sp.rnoise            = c.rnoise;
   sp.tinoise           = c.tinoise;
   sp.rinoise           = c.rinoise;
   sp.baseline          = 0.0;
   sp.sim               = true;

   sp.speed_step[ 0 ].duration_hours    = SIM_HOURS;
   sp.speed_step[ 0 ].duration_minutes  = 0.0;
   sp.speed_step[ 0 ].delay_hours       = 0;
   sp.speed_step[ 0 ].delay_minutes     = 0.0;
   sp.speed_step[ 0 ].scans             = SIM_SCANS;
   sp.speed_step[ 0 ].rotorspeed        = SIM_RPM;
   sp.speed_step[ 0 ].acceleration      = 400;
   sp.speed_step[ 0 ].acceleration_flag = false;
   sp.speed_step[ 0 ].set_speed         = SIM_RPM;
   sp.speed_step[ 0 ].avg_speed         = (double)SIM_RPM;

   return ( sp.save_simparms( path ) == 0 );
}

// Write the buffer XML one data set needs.
bool write_buffer( const QString& path, double d2o_pct )
{
   US_Buffer b;
   b.GUID        = US_Util::new_guid();
   b.description = QString( "3DSA harness, %1% D2O" )
                   .arg( d2o_pct, 0, 'f', 0 );
   b.pH          = 7.0;
   b.density     = d2o_density  ( d2o_pct );
   b.viscosity   = d2o_viscosity( d2o_pct );
   b.compressibility = 0.0;
   // Not "manual": the declared density and viscosity are 20 C values and
   // both the simulator and the fit apply the same temperature correction to
   // them.  The two sides must agree on this flag or their corrections drift.
   b.manual      = false;

   return b.writeToDisk( path );
}

// Write the model XML one data set needs.
//
// Standard (20W) space, deliberately: us_astfem_sim converts to experimental
// space itself, per component, using that component's own vbar20 and the
// buffer it is given.  Pre-converting here would apply the correction twice.
// Writing the truth in 20W is also what makes the harness a real round trip
// -- the simulator converts one way, the fit has to invert it.
//
// The only per-data-set difference is the cell loading, which scales the
// signal concentrations.
bool write_model( const QString& path, const GenCase& c,
                  const GenDataSet& ds )
{
   US_Model m;
   m.description = c.name;
   m.modelGUID   = US_Util::new_guid();
   m.analysis    = US_Model::MANUAL;

   for ( int ii = 0; ii < c.species.size(); ii++ )
   {
      const GenSpecies& sp = c.species[ ii ];

      US_Model::SimulationComponent comp;
      comp.s      = sp.s20w;
      comp.f_f0   = sp.ff0;
      comp.vbar20 = sp.vbar20;
      comp.D      = 0.0;
      comp.mw     = 0.0;
      comp.f      = 0.0;
      comp.signal_concentration = sp.conc * ds.loading;
      comp.name   = QString::asprintf( "SP%02d", ii + 1 );

      // Fill in D, mw and f from s, f/f0 and vbar; leave them in 20W space.
      if ( ! US_Model::calc_coefficients( comp ) )  return false;

      m.components << comp;
   }

   return ( m.write( path ) == 0 );
}

} // namespace

// ---------------------------------------------------------------------------
int US_3dsaCli::run_gen_cases( const QStringList& args )
{
   QString outdir;

   for ( int ii = 0; ii < args.size(); ii++ )
   {
      if ( args[ ii ] == "--outdir"  &&  ii + 1 < args.size() )
         outdir = args[ ++ii ];
      else
      {
         err() << "unknown gen-cases option: " << args[ ii ] << "\n";
         err().flush();
         return 2;
      }
   }

   if ( outdir.isEmpty() )
   {
      err() << "gen-cases needs --outdir <dir>\n";
      err().flush();
      return 2;
   }

   QDir().mkpath( outdir );
   QDir().mkpath( outdir + "/cases" );
   QDir().mkpath( outdir + "/inputs" );
   QDir().mkpath( outdir + "/data" );

   const QVector< GenCase > cases = build_cases();
   QJsonArray jcases;

   for ( const GenCase& c : cases )
   {
      const QString indir = outdir + "/inputs/" + c.name;
      QDir().mkpath( indir );

      QJsonArray jsteps;
      QJsonArray jdatasets;
      QJsonArray jspecies;

      for ( const GenSpecies& sp : c.species )
      {
         QJsonObject o;
         o[ "s20w" ]   = sp.s20w;
         o[ "ff0" ]    = sp.ff0;
         o[ "vbar20" ] = sp.vbar20;
         o[ "conc" ]   = sp.conc;
         jspecies << o;
      }

      // The nominal vbar the fit starts from: the concentration-weighted
      // truth would be cheating, so use the conventional protein value.
      const double nominal_vbar = TYPICAL_VBAR;

      for ( int ii = 0; ii < c.datasets.size(); ii++ )
      {
         const GenDataSet& ds = c.datasets[ ii ];
         const QString tag    = QString::asprintf( "ds%02d", ii );
         const QString runid  = c.name + "-" + tag;

         const QString mpath = QString( "%1/%2_model.xml"     ).arg( indir ).arg( tag );
         const QString bpath = QString( "%1/%2_buffer.xml"    ).arg( indir ).arg( tag );
         const QString spath = QString( "%1/%2_simparams.xml" ).arg( indir ).arg( tag );
         const QString ddir  = QString( "%1/data/%2" ).arg( outdir ).arg( runid );

         if ( ! write_model( mpath, c, ds ) )
         {
            err() << "cannot write " << mpath << "\n";  err().flush();
            return 3;
         }
         if ( ! write_buffer( bpath, ds.d2o_pct ) )
         {
            err() << "cannot write " << bpath << "\n";  err().flush();
            return 3;
         }
         if ( ! write_simparams( spath, c ) )
         {
            err() << "cannot write " << spath << "\n";  err().flush();
            return 3;
         }

         QDir().mkpath( ddir );

         QJsonObject step;
         step[ "runid" ]     = runid;
         step[ "model" ]     = mpath;
         step[ "buffer" ]    = bpath;
         step[ "simparams" ] = spath;
         step[ "outdir" ]    = ddir;
         jsteps << step;

         QJsonObject jd;
         // us_astfem_sim names the file <runid>.<type>.<cell>.<chan>.<wl>.auc
         jd[ "auc" ]         = QString( "%1/%2.RA.1.A.280.auc" )
                               .arg( ddir ).arg( runid );
         jd[ "simparams" ]   = spath;
         jd[ "label" ]       = tag + QString( " (%1% D2O)" )
                               .arg( ds.d2o_pct, 0, 'f', 0 );
         jd[ "d2o_percent" ] = ds.d2o_pct;
         jd[ "density" ]     = d2o_density  ( ds.d2o_pct );
         jd[ "viscosity" ]   = d2o_viscosity( ds.d2o_pct );
         jd[ "temperature" ] = SIM_TEMP;
         jd[ "loading" ]     = ds.loading;
         jd[ "vbar20" ]      = nominal_vbar;
         jdatasets << jd;
      }

      QJsonObject grid;
      grid[ "s_min" ] = 1.0e-13;  grid[ "s_max" ] = 10.0e-13;
      grid[ "s_res" ] = 19;       // step 0.5 S
      grid[ "k_min" ] = 1.0;      grid[ "k_max" ] = 3.0;
      grid[ "k_res" ] = 9;        // step 0.25
      grid[ "v_min" ] = c.v_min;  grid[ "v_max" ] = c.v_max;
      grid[ "v_res" ] = c.v_res;  // step 0.025
      grid[ "grid_reps" ] = 1;

      QJsonObject noise;
      noise[ "random" ] = c.rnoise;
      noise[ "ti" ]     = c.tinoise;
      noise[ "ri" ]     = c.rinoise;

      QJsonObject fit;
      fit[ "threads" ]         = 4;
      fit[ "noisflag" ]        = c.noisflag;
      fit[ "fit_scales" ]      = true;
      fit[ "edit_margin" ]     = 0.02;
      fit[ "ignore_contrast" ] = c.ignore_contrast;

      QJsonObject expect;
      expect[ "vbar_tol" ]        = c.vbar_tol;
      expect[ "s_tol" ]           = c.s_tol;
      expect[ "must_refuse" ]     = c.must_refuse;
      expect[ "rmsd_max" ]        = c.rmsd_max;
      expect[ "rmsd_spread_max" ] = c.rmsd_spread_max;

      QJsonObject jc;
      jc[ "name" ]        = c.name;
      jc[ "description" ] = c.description;
      jc[ "species" ]     = jspecies;
      jc[ "datasets" ]    = jdatasets;
      jc[ "noise" ]       = noise;
      jc[ "grid" ]        = grid;
      jc[ "fit" ]         = fit;
      jc[ "expect" ]      = expect;

      const QString cpath = QString( "%1/cases/%2.json" ).arg( outdir )
                            .arg( c.name );
      QFile cf( cpath );
      if ( ! cf.open( QIODevice::WriteOnly | QIODevice::Text ) )
      {
         err() << "cannot write " << cpath << "\n";  err().flush();
         return 3;
      }
      cf.write( QJsonDocument( jc ).toJson() );
      cf.close();

      QJsonObject jman;
      jman[ "name" ]        = c.name;
      jman[ "description" ] = c.description;
      jman[ "config" ]      = cpath;
      jman[ "sim_steps" ]   = jsteps;
      jcases << jman;
   }

   QJsonObject manifest;
   manifest[ "cases" ]     = jcases;
   manifest[ "generated" ] = QDateTime::currentDateTime().toString( Qt::ISODate );

   const QString mpath = outdir + "/manifest.json";
   QFile mf( mpath );
   if ( ! mf.open( QIODevice::WriteOnly | QIODevice::Text ) )
   {
      err() << "cannot write " << mpath << "\n";  err().flush();
      return 3;
   }
   mf.write( QJsonDocument( manifest ).toJson() );
   mf.close();

   out() << "wrote " << cases.size() << " cases to " << outdir << "\n";
   out() << "manifest: " << mpath << "\n";
   out().flush();
   return 0;
}

// ---------------------------------------------------------------------------
int US_3dsaCli::main( int argc, char** argv )
{
   QStringList args;
   for ( int ii = 1; ii < argc; ii++ )  args << QString::fromLocal8Bit( argv[ ii ] );

   if ( args.isEmpty()  ||  args[ 0 ] == "--help"  ||  args[ 0 ] == "-h" )
   {
      usage();
      return args.isEmpty() ? 2 : 0;
   }

   const QString cmd = args.takeFirst();

   if ( cmd == "fit" )        return run_fit      ( args );
   if ( cmd == "gen-cases" )  return run_gen_cases( args );

   err() << "unknown command: " << cmd << "\n";
   err().flush();
   usage();
   return 2;
}

int main( int argc, char** argv )
{
   QCoreApplication app( argc, argv );
   QCoreApplication::setApplicationName( "us_3dsa_cli" );
   return US_3dsaCli::main( argc, argv );
}
