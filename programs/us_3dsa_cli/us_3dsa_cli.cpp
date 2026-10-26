//! \file us_3dsa_cli.cpp
#include "us_3dsa_cli.h"

#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_buffer.h"
#include "us_dataIO.h"
#include "us_fvm_gradients.h"
#include "us_lamm_astfvm.h"
#include "us_math2.h"
#include "us_model.h"
#include "us_simparms.h"
#include "us_solute.h"
#include "us_util.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace
{

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

double       d2o_density( double pct )
{
   return H2O_DENSITY + ( D2O_DENSITY - H2O_DENSITY ) * pct / 100.0;
}

double d2o_viscosity( double pct )
{
   return H2O_VISCOSITY + ( D2O_VISCOSITY - H2O_VISCOSITY ) * pct / 100.0;
}

// Geometry shared by every generated case.  Small enough to keep the harness
// runnable in minutes, large enough to resolve a boundary.
const double SIM_MENISCUS = 5.8;
const double SIM_BOTTOM   = 7.2;
const double SIM_DELTA_R  = 0.005;
// Thirty scans.  Twelve resolved a boundary but left the fit short of the
// time information that separates s from D, and a real velocity run collects
// far more; the noise cases in particular were being asked to separate
// systematic noise from the model on the strength of a dozen time points.
const int    SIM_SCANS    = 30;
const int    SIM_RPM      = 45000;
const int    SIM_POINTS   = 200;
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

US_Math2::SolutionData corrections( double vbar20, double density, double viscosity, double temperature )
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
   out() << "us_3dsa_cli -- headless 3DSA fit driver\n\n"
         << "  us_3dsa_cli fit --config <case.json> [--out <result.json>]\n"
         << "                  [--model <model.xml>] [--curves <curves.csv>] [--threads N] [--quiet]\n\n"
         << "      Fit the buoyancy-contrast series the case file describes and\n"
         << "      report the global RMSD and the RMSD of every data set.\n\n"
         << "  us_3dsa_cli gen-cases --outdir <dir>\n\n"
         << "      Write the test-case tree: one JSON case file per case, the\n"
         << "      model / buffer / simulation-parameter XML each data set needs,\n"
         << "      and manifest.json naming the us_astfem_sim runs that produce\n"
         << "      the data.\n\n"
         << "  us_3dsa_cli simulate --config <case.json> [--seed N]\n\n"
         << "      Simulate the case's species into every data set's .auc, with\n"
         << "      the simulator its simulation parameters select.  An ASTFVM data\n"
         << "      set with a gradient-forming buffer (\"buffer\": a US_Buffer XML)\n"
         << "      is simulated in its gradient.  Scans are spread evenly over the\n"
         << "      speed step's duration.\n\n"
         << "  us_3dsa_cli gradient-from-interference --ip <interference.auc> --out <molar.auc>\n"
         << "         --meniscus <cm> --bottom <cm> --loading <g/mL> --dndc <mL/g> --molar-mass <g/mol>\n"
         << "         [--path <cm>] [--wavelength <nm>] [--stride N] [--dr <cm>]\n"
         << "         [--to-meniscus <cm> --to-bottom <cm>]\n\n"
         << "      Turn interference scans of a gradient former into its molar\n"
         << "      concentration over the run, conserving its mass over the cell,\n"
         << "      for a data set's \"gradient\".\n\n"
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
      const US_Model::SimulationComponent& c = m.components[cc];
      const double                         w = c.signal_concentration;

      double                               v = 0.0;
      switch ( attr )
      {
         case US_Solute::ATTR_S:
            v = c.s;
            break;
         case US_Solute::ATTR_K:
            v = c.f_f0;
            break;
         case US_Solute::ATTR_V:
            v = c.vbar20;
            break;
         case US_Solute::ATTR_D:
            v = c.D;
            break;
         default:
            v = c.mw;
            break;
      }

      num += v * w;
      den += w;
   }

   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

double US_3dsaCli::truth_vbar( const Case& kase )
{
   double num = 0.0, den = 0.0;
   for ( const Species& s : kase.species )
   {
      num += s.vbar20 * s.conc;
      den += s.conc;
   }
   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

double US_3dsaCli::truth_s( const Case& kase )
{
   double num = 0.0, den = 0.0;
   for ( const Species& s : kase.species )
   {
      num += s.s20w * s.conc;
      den += s.conc;
   }
   return ( den > 0.0 ) ? ( num / den ) : 0.0;
}

// ---------------------------------------------------------------------------
bool US_3dsaCli::read_case( const QString& path, Case& kase, QString& emsg )
{
   QFile f( path );

   if ( !f.open( QIODevice::ReadOnly | QIODevice::Text ) )
   {
      emsg = QString( "cannot open %1" ).arg( path );
      return false;
   }

   QJsonParseError     perr;
   const QJsonDocument doc = QJsonDocument::fromJson( f.readAll(), &perr );
   f.close();

   if ( doc.isNull() || !doc.isObject() )
   {
      emsg = QString( "%1: %2" ).arg( path ).arg( perr.errorString() );
      return false;
   }

   const QJsonObject root = doc.object();
   kase.name              = root.value( "name" ).toString();
   kase.description       = root.value( "description" ).toString();

   for ( const QJsonValue& v : root.value( "species" ).toArray() )
   {
      const QJsonObject o = v.toObject();
      Species           sp;
      sp.s20w   = jnum( o, "s20w", 0.0 );
      sp.ff0    = jnum( o, "ff0", 1.0 );
      sp.vbar20 = jnum( o, "vbar20", 0.72 );
      sp.conc   = jnum( o, "conc", 1.0 );
      kase.species << sp;
   }

   for ( const QJsonValue& v : root.value( "datasets" ).toArray() )
   {
      const QJsonObject o = v.toObject();
      DataSetSpec       ds;
      ds.auc         = o.value( "auc" ).toString();
      ds.simparams   = o.value( "simparams" ).toString();
      ds.label       = o.value( "label" ).toString();
      ds.d2o_percent = jnum( o, "d2o_percent", 0.0 );
      ds.density     = jnum( o, "density", H2O_DENSITY );
      ds.viscosity   = jnum( o, "viscosity", H2O_VISCOSITY );
      ds.temperature = jnum( o, "temperature", SIM_TEMP );
      ds.loading     = jnum( o, "loading", 1.0 );
      ds.vbar20      = jnum( o, "vbar20", 0.72 );
      ds.rnoise      = jnum( o, "rnoise", 0.0 );
      ds.tinoise     = jnum( o, "tinoise", 0.0 );
      ds.rinoise     = jnum( o, "rinoise", 0.0 );
      ds.rmsd_max    = jnum( o, "rmsd_max", 0.0 );
      ds.run_dir        = o.value( "run_dir" ).toString();
      ds.run_id         = o.value( "run_id" ).toString();
      ds.edit           = o.value( "edit" ).toString();
      ds.buffer         = o.value( "buffer" ).toString();
      ds.has_density    = o.contains( "density" );
      ds.has_viscosity  = o.contains( "viscosity" );
      ds.last_scan_only = jbool( o, "last_scan_only", false );
      ds.gradient        = o.value( "gradient" ).toString();
      ds.gradient_former = o.value( "gradient_former" ).toString();
      kase.datasets << ds;
   }

   const QJsonObject noi = root.value( "noise" ).toObject();
   kase.rnoise           = jnum( noi, "random", 0.0 );
   kase.tinoise          = jnum( noi, "ti", 0.0 );
   kase.rinoise          = jnum( noi, "ri", 0.0 );

   const QJsonObject g = root.value( "grid" ).toObject();
   kase.s_min          = jnum( g, "s_min", kase.s_min );
   kase.s_max          = jnum( g, "s_max", kase.s_max );
   kase.s_res          = jint( g, "s_res", kase.s_res );
   kase.k_min          = jnum( g, "k_min", kase.k_min );
   kase.k_max          = jnum( g, "k_max", kase.k_max );
   kase.k_res          = jint( g, "k_res", kase.k_res );
   kase.v_min          = jnum( g, "v_min", kase.v_min );
   kase.v_max          = jnum( g, "v_max", kase.v_max );
   kase.v_res          = jint( g, "v_res", kase.v_res );
   kase.grid_reps      = jint( g, "grid_reps", kase.grid_reps );
   kase.mask           = g.contains( "mask" ) ? g.value( "mask" ).toString() : kase.mask;
   kase.w_min          = jnum( g, "w_min", kase.w_min );
   kase.w_max          = jnum( g, "w_max", kase.w_max );
   kase.w_res          = jint( g, "w_res", kase.w_res );
   for ( const QJsonValue& v : g.value( "v_list" ).toArray() )
   {
      kase.v_list << v.toDouble();
   }

   const QJsonObject fo    = root.value( "fit" ).toObject();
   kase.threads            = jint( fo, "threads", kase.threads );
   kase.noisflag           = jint( fo, "noisflag", kase.noisflag );
   kase.fit_scales         = jbool( fo, "fit_scales", kase.fit_scales );
   kase.edit_margin        = jnum( fo, "edit_margin", kase.edit_margin );
   kase.edit_bottom_margin = jnum( fo, "edit_bottom_margin", kase.edit_bottom_margin );
   kase.ignore_contrast    = jbool( fo, "ignore_contrast", kase.ignore_contrast );
   kase.decoy_vbar         = jnum( fo, "decoy_vbar", kase.decoy_vbar );
   kase.equilibrium        = jbool( fo, "equilibrium", kase.equilibrium );
   kase.verify             = jbool( fo, "verify", kase.verify );

   if ( kase.mask != "s_k_v" && kase.mask != "s_d_v" && kase.mask != "w_k_v" )
   {
      emsg = QString( "%1: unknown grid mask \"%2\"" ).arg( path ).arg( kase.mask );
      return false;
   }

   const QJsonObject ex = root.value( "expect" ).toObject();
   kase.vbar_tol        = jnum( ex, "vbar_tol", kase.vbar_tol );
   kase.s_tol           = jnum( ex, "s_tol", kase.s_tol );
   kase.must_refuse     = jbool( ex, "must_refuse", kase.must_refuse );
   kase.rmsd_max        = jnum( ex, "rmsd_max", kase.rmsd_max );
   kase.rmsd_spread_max = jnum( ex, "rmsd_spread_max", kase.rmsd_spread_max );

   if ( kase.datasets.isEmpty() )
   {
      emsg = QString( "%1: no datasets" ).arg( path );
      return false;
   }

   return true;
}

// ---------------------------------------------------------------------------
bool US_3dsaCli::load_datasets( const Case& kase, QList<US_SolveSim::DataSet*>& dsets, QString& emsg )
{
   const double edit_margin  = kase.edit_margin;
   const double edit_bmargin = ( kase.edit_bottom_margin > 0.0 ) ? kase.edit_bottom_margin : kase.edit_margin;

   for ( int ii = 0; ii < kase.datasets.size(); ii++ )
   {
      const DataSetSpec&    spec = kase.datasets[ii];
      US_SolveSim::DataSet* d    = new US_SolveSim::DataSet;

      // Simulation parameters: the same file the data was generated from, so
      // the fit's forward model matches the simulator exactly.
      if ( !spec.simparams.isEmpty() )
      {
         if ( d->simparams.load_simparms( spec.simparams ) != 0 )
         {
            emsg = QString( "cannot read %1" ).arg( spec.simparams );
            delete d;
            return false;
         }
      }

      if ( !spec.edit.isEmpty() )
      {
         // Real data, edited: what us_edit kept is what the fit sees
         if ( !load_edited( spec.edit, d->run_data, emsg ) )
         {
            delete d;
            return false;
         }

         d->simparams.meniscus = d->run_data.meniscus;
         if ( d->run_data.bottom > 0.0 )
         {
            d->simparams.bottom = d->run_data.bottom;
         }

         // The run's timestate sits beside its data
         const QFileInfo efi( spec.edit );
         const QString   tmst = efi.dir().filePath( d->run_data.runID + ".time_state.tmst" );

         if ( QFileInfo( tmst ).isFile() )
         {
            d->tmst_file = tmst;
         }
      }
      else if ( !load_trimmed( spec, edit_margin, edit_bmargin, d, emsg ) )
      {
         delete d;
         return false;
      }

      if ( spec.last_scan_only && d->run_data.scanData.size() > 1 )
      {
         d->run_data.scanData.remove( 0, d->run_data.scanData.size() - 1 );
      }

      US_SimulationParameters::computeSpeedSteps( &d->run_data.scanData, d->simparams.speed_step );

      double density   = spec.density;
      double viscosity = spec.viscosity;

      if ( !spec.buffer.isEmpty() )
      {
         if ( !d->solution_rec.buffer.readFromDisk( spec.buffer ) )
         {
            emsg = QString( "cannot read buffer %1" ).arg( spec.buffer );
            delete d;
            return false;
         }

         density   = spec.has_density ? density : d->solution_rec.buffer.density;
         viscosity = spec.has_viscosity ? viscosity : d->solution_rec.buffer.viscosity;
      }

      if ( !spec.gradient.isEmpty() )
      {
         US_DataIO::RawData molar;
         QString            gmsg;

         if ( US_DataIO::readRawData( spec.gradient, molar ) != US_DataIO::OK )
         {
            emsg = QString( "cannot read gradient %1" ).arg( spec.gradient );
            delete d;
            return false;
         }

         QSharedPointer<US_FvmGradients> grads( new US_FvmGradients );

         if ( !grads->prepare_measured( d->solution_rec.buffer, spec.gradient_former, molar, gmsg ) )
         {
            emsg = QString( "%1: %2" ).arg( spec.gradient ).arg( gmsg );
            delete d;
            return false;
         }

         d->fvm = grads;
      }

      d->density     = density;
      d->viscosity   = viscosity;
      d->temperature = spec.temperature;
      d->manual      = false;
      d->compress    = 0.0;
      d->vbar20      = spec.vbar20;

      US_Math2::SolutionData sd = corrections( spec.vbar20, density, viscosity, spec.temperature );
      d->vbartb                 = sd.vbar;
      d->s20w_correction        = sd.s20w_correction;
      d->D20w_correction        = sd.D20w_correction;
      d->centerpiece_bottom     = d->simparams.bottom;

      // The run's own timestate, when the simulator wrote one next to the
      // data (us_astfem_sim saves <run_id>.time_state.tmst in the run
      // directory), as a real analysis would use it.  Without one the engine
      // synthesizes a timestate per data set from the scans.
      if ( !spec.run_dir.isEmpty() && !spec.run_id.isEmpty() )
      {
         const QString tmst = QDir( spec.run_dir ).filePath( spec.run_id + ".time_state.tmst" );

         if ( QFileInfo( tmst ).isFile() )
         {
            d->tmst_file = tmst;
         }
      }

      dsets << d;
   }

   return true;
}

// ---------------------------------------------------------------------------
// A synthetic data set: the whole simulated cell, trimmed to an edited range.
bool US_3dsaCli::load_trimmed( const DataSetSpec&    spec,
                               double                edit_margin,
                               double                edit_bmargin,
                               US_SolveSim::DataSet* d,
                               QString&              emsg )
{
   US_DataIO::RawData raw;
   const int          rc = US_DataIO::readRawData( spec.auc, raw );

   if ( rc != US_DataIO::OK )
   {
      emsg = QString( "cannot read %1 (code %2)" ).arg( spec.auc ).arg( rc );
      return false;
   }

   if ( raw.scanCount() < 1 || raw.pointCount() < 1 )
   {
      emsg = QString( "%1 holds no scans" ).arg( spec.auc );
      return false;
   }

   d->run_data.runID      = QFileInfo( spec.auc ).completeBaseName();
   d->run_data.dataType   = "RA";
   d->run_data.cell       = QString::number( raw.cell );
   d->run_data.channel    = QString( QChar( raw.channel ) );
   d->run_data.wavelength = QString::number( qRound( raw.scanData[0].wavelength ) );
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
   //
   // The bottom margin is the larger of the two: that is where the
   // boundary piles up, and it matches what us_astfem_sim writes into the
   // edit profile it registers alongside the data.  The harness rewrites
   // that profile to exactly this window, so a 2DSA cross-check analyses
   // the same radii this fit does.
   const double r_lo = d->simparams.meniscus + edit_margin;
   const double r_hi = d->simparams.bottom - edit_bmargin;

   QVector<int> keep;
   keep.reserve( raw.xvalues.size() );

   for ( int rr = 0; rr < raw.xvalues.size(); rr++ )
   {
      if ( raw.xvalues[rr] >= r_lo && raw.xvalues[rr] <= r_hi )
      {
         keep << rr;
      }
   }

   if ( keep.size() < 10 )
   {
      emsg = QString( "%1: only %2 points survive the %3 / %4 cm edit"
                      " margins" )
                .arg( spec.auc )
                .arg( keep.size() )
                .arg( edit_margin )
                .arg( edit_bmargin );
      return false;
   }

   d->run_data.xvalues.clear();
   d->run_data.xvalues.reserve( keep.size() );
   for ( int rr : keep )
   {
      d->run_data.xvalues << raw.xvalues[rr];
   }

   d->run_data.scanData.clear();
   d->run_data.scanData.reserve( raw.scanData.size() );

   for ( int ss = 0; ss < raw.scanData.size(); ss++ )
   {
      US_DataIO::Scan sc = raw.scanData[ss];
      sc.rvalues.clear();
      sc.rvalues.reserve( keep.size() );
      for ( int rr : keep )
      {
         sc.rvalues << raw.scanData[ss].rvalues[rr];
      }
      d->run_data.scanData << sc;
   }

   return true;
}

// ---------------------------------------------------------------------------
// Real data through its edit profile.  US_DataIO::loadData reports a failure
// by throwing its error code.
bool US_3dsaCli::load_edited( const QString& edit_path, US_DataIO::EditedData& ed, QString& emsg )
{
   const QFileInfo efi( edit_path );

   try
   {
      const int rc = US_DataIO::loadData( efi.absolutePath(), efi.fileName(), ed );

      if ( rc != US_DataIO::OK )
      {
         emsg = QString( "cannot load %1: %2" ).arg( edit_path ).arg( US_DataIO::errorString( rc ) );
         return false;
      }
   }
   catch ( US_DataIO::ioError rc )
   {
      emsg = QString( "cannot load %1: %2" ).arg( edit_path ).arg( US_DataIO::errorString( rc ) );
      return false;
   }

   if ( ed.scanData.isEmpty() || ed.xvalues.size() < 10 )
   {
      emsg = QString( "%1 keeps %2 scans of %3 points" ).arg( edit_path ).arg( ed.scanData.size() ).arg( ed.xvalues.size() );
      return false;
   }

   // The linear baseline us_edit fitted, which the analyses subtract
   if ( ed.bl_corr_slope != 0.0 || ed.bl_corr_yintercept != 0.0 )
   {
      for ( US_DataIO::Scan& sc : ed.scanData )
      {
         for ( int jj = 0; jj < sc.rvalues.size(); jj++ )
         {
            sc.rvalues[jj] -= ed.xvalues[jj] * ed.bl_corr_slope + ed.bl_corr_yintercept;
         }
      }
   }

   return true;
}

// ---------------------------------------------------------------------------
// The fitted solutes, simulated by the Lamm-equation solver (the ASTFVM
// where the data set selects it) instead of the closed form, with their
// amplitudes refitted by NNLS.  An RMSD near the closed form's says the
// solutes had reached equilibrium in their gradient; one well above it says
// the run was too short for the closed form.  A solve can take many seconds,
// so only the solutes carrying the signal are checked: the largest, down to
// half a percent each, at most VERIFY_MAX of them.
double US_3dsaCli::verify_with_solver( QList<US_SolveSim::DataSet*>& dsets,
                                       const US_3dsaProcess::Result& result,
                                       int                           noisflag,
                                       int&                          nchecked,
                                       double&                       share,
                                       double&                       closed_rmsd )
{
   const int                            VERIFY_MAX = 16;
   QList<US_Model::SimulationComponent> comps      = result.model.components.toList();
   std::sort( comps.begin(),
              comps.end(),
              []( const US_Model::SimulationComponent& a, const US_Model::SimulationComponent& b )
              { return a.signal_concentration > b.signal_concentration; } );

   double total = 0.0;
   for ( const US_Model::SimulationComponent& c : comps )
   {
      total += c.signal_concentration;
   }

   QList<US_Model::SimulationComponent> checked;
   double                               kept = 0.0;
   for ( const US_Model::SimulationComponent& c : comps )
   {
      if ( checked.size() >= VERIFY_MAX || c.signal_concentration < 0.005 * total )
      {
         break;
      }
      checked << c;
      kept += c.signal_concentration;
   }

   nchecked = checked.size();
   share    = ( total > 0.0 ) ? kept / total : 0.0;

   QString       msg;
   QTemporaryDir tdir;

   if ( !prepare_refit( dsets, tdir.path(), msg ) )
   {
      err() << "verify: " << msg << "\n";
      err().flush();
      return -1.0;
   }

   // The same solutes fitted both ways, so a subset that leaves part of the
   // signal unexplained costs both fits alike
   US_SolveSim::Simulation closed;
   US_SolveSim::Simulation solver;
   refit_solutes( dsets, checked, noisflag, result.scales, true, closed );
   refit_solutes( dsets, checked, noisflag, result.scales, false, solver );
   closed_rmsd = sqrt( qMax( 0.0, closed.variance ) );
   return sqrt( qMax( 0.0, solver.variance ) );
}

// ---------------------------------------------------------------------------
// Data sets ready for refit_solutes(): the fit leaves the callers' own
// without timestates or gradients
bool US_3dsaCli::prepare_refit( QList<US_SolveSim::DataSet*>& dsets, const QString& tmst_dir, QString& msg )
{
   return US_3dsaProcess::prepare_timestates( dsets, tmst_dir, msg ) && US_3dsaProcess::prepare_gradients( dsets, msg );
}

// Given solutes refitted to the data, their concentrations by NNLS, with the
// closed form or the Lamm-equation solver.  sv returns the fit, its
// simulation and its residuals.
void US_3dsaCli::refit_solutes( QList<US_SolveSim::DataSet*>&               dsets,
                                const QList<US_Model::SimulationComponent>& comps,
                                int                                         noisflag,
                                const QVector<double>&                      scales,
                                bool                                        equilibrium,
                                US_SolveSim::Simulation&                    sv )
{
   for ( US_SolveSim::DataSet* dset : dsets )
   {
      dset->solute_type = US_3dsaProcess::mask_s_k_v();
      dset->fit_vbar    = true;
      dset->equilibrium = equilibrium;
   }

   sv.solutes.clear();
   for ( const US_Model::SimulationComponent& c : comps )
   {
      sv.solutes << US_Solute( c.s, c.f_f0, 0.0, c.vbar20, c.D );
   }
   sv.noisflag   = noisflag;
   sv.dbg_level  = 0;
   sv.dbg_timing = false;
   sv.scales     = scales;

   US_SolveSim solver( dsets, 0, false );
   solver.calc_residuals( 0, dsets.size(), sv );
}

// ---------------------------------------------------------------------------
// The fit's curves, for plotting: per data set and scan, each radius with the
// data, the fit (model plus fitted noise) and the residual
bool US_3dsaCli::write_curves( const QString&                path,
                               QList<US_SolveSim::DataSet*>& dsets,
                               const Case&                   kase,
                               const US_3dsaProcess::Result& result,
                               double&                       rmsd )
{
   QString       msg;
   QTemporaryDir tdir;

   if ( !prepare_refit( dsets, tdir.path(), msg ) )
   {
      err() << "curves: " << msg << "\n";
      err().flush();
      return false;
   }

   US_SolveSim::Simulation sv;
   refit_solutes( dsets, result.model.components.toList(), kase.noisflag, result.scales, kase.equilibrium, sv );
   rmsd = sqrt( qMax( 0.0, sv.variance ) );

   QFile f( path );

   if ( !f.open( QIODevice::WriteOnly | QIODevice::Text ) )
   {
      err() << "curves: cannot write " << path << "\n";
      err().flush();
      return false;
   }

   QTextStream ts( &f );
   ts << "dataset,label,scan,seconds,radius,data,fit,residual\n";
   int scnx = 0;

   for ( int dd = 0; dd < dsets.size(); dd++ )
   {
      const US_DataIO::EditedData& ed    = dsets[dd]->run_data;
      const QString                label = kase.datasets[dd].label;

      for ( int ss = 0; ss < ed.scanData.size(); ss++, scnx++ )
      {
         for ( int rr = 0; rr < ed.xvalues.size(); rr++ )
         {
            const double data  = ed.scanData[ss].rvalues[rr];
            const double resid = sv.residuals.value( scnx, rr );
            ts << dd << ",\"" << label << "\"," << ss << "," << ed.scanData[ss].seconds << ","
               << QString::number( ed.xvalues[rr], 'f', 5 ) << "," << QString::number( data, 'g', 8 ) << ","
               << QString::number( data - resid, 'g', 8 ) << "," << QString::number( resid, 'g', 8 ) << "\n";
         }
      }
   }

   return true;
}

// ---------------------------------------------------------------------------
int US_3dsaCli::run_fit( const QStringList& args )
{
   QString cfg_path;
   QString out_path;
   QString model_path;
   QString curves_path;
   int     threads = -1;
   bool    quiet   = false;

   for ( int ii = 0; ii < args.size(); ii++ )
   {
      const QString a = args[ii];

      if ( a == "--config" && ii + 1 < args.size() )
      {
         cfg_path = args[++ii];
      }
      else if ( a == "--out" && ii + 1 < args.size() )
      {
         out_path = args[++ii];
      }
      else if ( a == "--model" && ii + 1 < args.size() )
      {
         model_path = args[++ii];
      }
      else if ( a == "--curves" && ii + 1 < args.size() )
      {
         curves_path = args[++ii];
      }
      else if ( a == "--threads" && ii + 1 < args.size() )
      {
         threads = args[++ii].toInt();
      }
      else if ( a == "--quiet" )
      {
         quiet = true;
      }
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

   if ( !read_case( cfg_path, kase, emsg ) )
   {
      err() << "case error: " << emsg << "\n";
      err().flush();
      return 2;
   }

   if ( threads > 0 )
   {
      kase.threads = threads;
   }

   QList<US_SolveSim::DataSet*> dsets;

   if ( !load_datasets( kase, dsets, emsg ) )
   {
      err() << "data error: " << emsg << "\n";
      err().flush();
      qDeleteAll( dsets );
      return 3;
   }

   US_3dsaProcess::Parameters p;
   p.x_min           = kase.s_min;
   p.x_max           = kase.s_max;
   p.x_res           = kase.s_res;
   p.y_min           = kase.k_min;
   p.y_max           = kase.k_max;
   p.y_res           = kase.k_res;
   p.z_min           = kase.v_min;
   p.z_max           = kase.v_max;
   p.z_res           = kase.v_res;
   p.grid_reps       = kase.grid_reps;
   p.s_mask          = US_3dsaProcess::mask_s_k_v();
   p.nthreads        = kase.threads;
   p.noisflag        = kase.noisflag;
   p.fit_scales      = kase.fit_scales;
   p.ignore_contrast = kase.ignore_contrast;
   p.equilibrium     = kase.equilibrium;

   if ( kase.mask == "s_d_v" )
   {
      p.s_mask = US_3dsaProcess::mask_s_d_v();
   }
   else if ( kase.mask == "w_k_v" )
   {
      p.s_mask = US_3dsaProcess::mask_w_k_v();
      p.x_min  = kase.w_min;
      p.x_max  = kase.w_max;
      p.x_res  = kase.w_res;
   }

   if ( !kase.v_list.isEmpty() )
   {
      // The x and y axes as gridded, crossed with the listed vbar values
      for ( double vv : kase.v_list )
      {
         for ( int jj = 0; jj < p.y_res; jj++ )
         {
            const double yy = ( p.y_res > 1 ) ? p.y_min + ( p.y_max - p.y_min ) * jj / ( p.y_res - 1 ) : p.y_min;

            for ( int ii = 0; ii < p.x_res; ii++ )
            {
               const double xx = ( p.x_res > 1 ) ? p.x_min + ( p.x_max - p.x_min ) * ii / ( p.x_res - 1 ) : p.x_min;
               US_Solute    solute( 0.0, yy, 0.0, vv, 0.0 );

               if ( kase.mask == "w_k_v" )
               {
                  solute.d = xx;
               }
               else
               {
                  solute.s = xx;
               }

               if ( kase.mask == "s_d_v" )
               {
                  solute.k = 0.0;
                  solute.d = yy;
               }

               p.solutes << solute;
            }
         }
      }
   }

   US_3dsaProcess         proc( dsets );
   US_3dsaProcess::Result result;

   if ( !quiet )
   {
      QObject::connect( &proc,
                        &US_3dsaProcess::message_update,
                        []( const QString& m )
                        {
                           out() << "  | " << m << "\n";
                           out().flush();
                        } );
   }

   out() << "=== " << kase.name << " ===\n";
   if ( !kase.description.isEmpty() )
   {
      out() << "  " << kase.description << "\n";
   }
   out() << QString( "  %1 data sets, %2 species, grid %3 x %4 x %5,"
                     " reps %6\n" )
               .arg( kase.datasets.size() )
               .arg( kase.species.size() )
               .arg( kase.s_res )
               .arg( kase.k_res )
               .arg( kase.v_res )
               .arg( kase.grid_reps );
   out().flush();

   const bool  ok = proc.fit( p, result );

   QJsonObject jr;
   jr["case"]     = kase.name;
   jr["accepted"] = ok;

   if ( !ok )
   {
      out() << "  REFUSED: " << proc.lastError() << "\n";
      jr["error"] = proc.lastError();

      const bool as_expected = kase.must_refuse;
      out() << ( as_expected ? "  PASS (refusal expected)\n" : "  FAIL (refusal not expected)\n" );
      jr["pass"] = as_expected;

      if ( !out_path.isEmpty() )
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
      jr["pass"] = false;
   }

   // ---- report ------------------------------------------------------------
   const double got_vbar = weighted( result.model, US_Solute::ATTR_V );
   const double got_s    = weighted( result.model, US_Solute::ATTR_S );
   const double exp_vbar = truth_vbar( kase );
   const double exp_s    = truth_s( kase );

   out() << "\n";
   out() << QString( "  components %1   depth levels %2   NNLS solves %3"
                     "   %6 %4   %5 s\n" )
               .arg( result.model.components.size() )
               .arg( result.ndepths )
               .arg( result.ntasks )
               .arg( result.nsimul )
               .arg( result.msecs / 1000.0, 0, 'f', 1 )
               .arg( kase.equilibrium ? "equilibrium profiles" : "Lamm solves" );
   out() << QString( "  contrast %1 (mL/g)^-1, implied vbar resolution"
                     " %2 mL/g, %3 amplitude iterations\n" )
               .arg( result.contrast, 0, 'f', 3 )
               .arg( result.vbar_resol, 0, 'f', 4 )
               .arg( result.nscaliter );
   out() << QString( "  threads requested %1, used %2%3\n" )
               .arg( kase.threads )
               .arg( result.nthreads )
               .arg( result.nthreads < kase.threads && !US_3dsaProcess::concurrent_solves_safe( dsets, p )
                        ? QString( " (this build cannot run concurrent Lamm solves)" )
                        : QString( "" ) );

   out() << "\n  GLOBAL RMSD  " << QString::number( result.rmsd, 'e', 6 ) << "\n\n";

   out() << QString( "  %1 %2 %3 %4 %5 %6 %7 %8\n" )
               .arg( "data set", -22 )
               .arg( "D2O pct", 8 )
               .arg( "density", 12 )
               .arg( "RMSD", 12 )
               .arg( "scale fit", 12 )
               .arg( "scale true", 12 )
               .arg( "noise inj", 12 )
               .arg( "RMSD cap", 12 );

   double rmsd_lo = 1.0e+300;
   double rmsd_hi = 0.0;
   int    ds_over = 0; // data sets above their own RMSD cap

   for ( int ii = 0; ii < kase.datasets.size(); ii++ )
   {
      const DataSetSpec& ds   = kase.datasets[ii];
      const double       var  = ( ii < result.variances.size() ) ? result.variances[ii] : 0.0;
      const double       rmsd = sqrt( qMax( 0.0, var ) );
      const double       sc   = ( ii < result.scales.size() ) ? result.scales[ii] : 1.0;
      const double       sct =
         ( !kase.datasets.isEmpty() && kase.datasets[0].loading > 0.0 ) ? ds.loading / kase.datasets[0].loading : 1.0;

      rmsd_lo = qMin( rmsd_lo, rmsd );
      rmsd_hi = qMax( rmsd_hi, rmsd );

      // What this cell was given.  Random noise is not fitted, so it sets
      // the floor its residual can reach; TI and RI are fitted, so they
      // should leave nothing behind.
      const double injected = ds.rnoise;
      const bool   ds_ok    = ( ds.rmsd_max <= 0.0 ) || ( rmsd <= ds.rmsd_max );

      if ( !ds_ok )
      {
         ds_over++;
      }

      out() << QString( "  %1 %2 %3 %4 %5 %6 %7 %8%9\n" )
                  .arg( ds.label.isEmpty() ? QFileInfo( ds.auc ).fileName() : ds.label, -22 )
                  .arg( ds.d2o_percent, 8, 'f', 1 )
                  .arg( ds.density, 12, 'f', 6 )
                  .arg( QString::number( rmsd, 'e', 4 ), 12 )
                  .arg( sc, 12, 'f', 5 )
                  .arg( sct, 12, 'f', 5 )
                  .arg( QString::number( injected, 'e', 2 ), 12 )
                  .arg( ds.rmsd_max > 0.0 ? QString::number( ds.rmsd_max, 'e', 2 ) : QString( "-" ), 12 )
                  .arg( ds_ok ? QString( "" ) : QString( "  ** OVER **" ) );

      QJsonObject jd;
      jd["label"]                           = ds.label;
      jd["d2o_percent"]                     = ds.d2o_percent;
      jd["density"]                         = ds.density;
      jd["rmsd"]                            = rmsd;
      jd["variance"]                        = var;
      jd["scale_fit"]                       = sc;
      jd["scale_true"]                      = sct;
      jd["rnoise"]                          = ds.rnoise;
      jd["tinoise"]                         = ds.tinoise;
      jd["rinoise"]                         = ds.rinoise;
      jd["rmsd_max"]                        = ds.rmsd_max;
      jd["rmsd_ok"]                         = ds_ok;
      jr[QString( "dataset_%1" ).arg( ii )] = jd;
   }

   out() << "\n";
   out() << QString( "  weighted vbar   fit %1   true %2   error %3\n" )
               .arg( got_vbar, 0, 'f', 5 )
               .arg( exp_vbar, 0, 'f', 5 )
               .arg( got_vbar - exp_vbar, 0, 'f', 5 );
   out() << QString( "  weighted s (S)  fit %1   true %2   error %3\n" )
               .arg( got_s * 1.0e13, 0, 'f', 4 )
               .arg( exp_s * 1.0e13, 0, 'f', 4 )
               .arg( ( got_s - exp_s ) * 1.0e13, 0, 'f', 4 );

   const double spread = ( rmsd_lo > 0.0 ) ? ( rmsd_hi / rmsd_lo ) : 0.0;

   const bool   vbar_ok   = ( fabs( got_vbar - exp_vbar ) <= kase.vbar_tol );
   const bool   s_ok      = ( fabs( got_s - exp_s ) <= kase.s_tol );
   const bool   rmsd_ok   = ( kase.rmsd_max <= 0.0 ) || ( result.rmsd <= kase.rmsd_max );
   const bool   spread_ok = ( kase.rmsd_spread_max <= 0.0 ) || ( spread <= kase.rmsd_spread_max );
   // A series whose cells were given different noise has no single meaningful
   // RMSD and no meaningful spread; it caps each cell separately instead.
   const bool   dssets_ok = ( ds_over == 0 );
   const bool   pass      = vbar_ok && s_ok && rmsd_ok && spread_ok && dssets_ok && !kase.must_refuse;

   out() << QString( "  per-data-set RMSD spread  %1x"
                     "  (largest / smallest)\n" )
               .arg( spread, 0, 'f', 1 );

   out() << ( pass ? "  PASS\n" : "  FAIL\n" );
   if ( !vbar_ok )
   {
      out() << QString( "    vbar error %1 exceeds tolerance %2\n" )
                  .arg( fabs( got_vbar - exp_vbar ), 0, 'f', 5 )
                  .arg( kase.vbar_tol, 0, 'f', 5 );
   }
   if ( !s_ok )
   {
      out() << QString( "    s error %1 S exceeds tolerance %2 S\n" )
                  .arg( fabs( got_s - exp_s ) * 1.0e13, 0, 'f', 4 )
                  .arg( kase.s_tol * 1.0e13, 0, 'f', 4 );
   }
   if ( !rmsd_ok )
   {
      out() << QString( "    global RMSD %1 exceeds cap %2 -- the fit did not"
                        " reach the residual this case should allow\n" )
                  .arg( QString::number( result.rmsd, 'e', 4 ) )
                  .arg( QString::number( kase.rmsd_max, 'e', 4 ) );
   }
   if ( !spread_ok )
   {
      out() << QString( "    per-data-set RMSD spread %1x exceeds cap %2x --"
                        " the fit is treating the data sets unequally\n" )
                  .arg( spread, 0, 'f', 1 )
                  .arg( kase.rmsd_spread_max, 0, 'f', 1 );
   }
   if ( !dssets_ok )
   {
      out() << QString( "    %1 data set(s) above their own RMSD cap -- with"
                        " noise varying by cell, each cell's residual has to"
                        " reach the level that cell was given\n" )
                  .arg( ds_over );
   }
   out() << "\n";
   out().flush();

   // ---- the solutes found ---------------------------------------------------
   {
      QList<US_Model::SimulationComponent> comps = result.model.components.toList();
      std::sort( comps.begin(),
                 comps.end(),
                 []( const US_Model::SimulationComponent& a, const US_Model::SimulationComponent& b )
                 { return a.vbar20 > b.vbar20; } );

      double total = 0.0;
      for ( const US_Model::SimulationComponent& c : comps )
      {
         total += c.signal_concentration;
      }

      out() << QString( "  %1 %2 %3 %4 %5 %6 %7\n" )
                  .arg( "s20w (S)", 10 )
                  .arg( "f/f0", 7 )
                  .arg( "M (kDa)", 11 )
                  .arg( "vbar20", 8 )
                  .arg( "1/vbar", 8 )
                  .arg( "signal", 10 )
                  .arg( "share", 7 );

      for ( const US_Model::SimulationComponent& c : comps )
      {
         out() << QString( "  %1 %2 %3 %4 %5 %6 %7\n" )
                     .arg( c.s * 1.0e13, 10, 'f', 2 )
                     .arg( c.f_f0, 7, 'f', 3 )
                     .arg( c.mw / 1000.0, 11, 'f', 1 )
                     .arg( c.vbar20, 8, 'f', 4 )
                     .arg( c.vbar20 > 0.0 ? 1.0 / c.vbar20 : 0.0, 8, 'f', 4 )
                     .arg( c.signal_concentration, 10, 'f', 5 )
                     .arg( total > 0.0 ? QString( "%1%" ).arg( 100.0 * c.signal_concentration / total, 0, 'f', 1 ) : QString( "-" ),
                           7 );
      }

      out() << "\n";
      out().flush();
   }

   // ---- the closed form, checked by the Lamm-equation solver ----------------
   if ( kase.equilibrium && kase.verify )
   {
      int          nchecked = 0;
      double       share    = 0.0;
      double       crmsd    = 0.0;
      const double vrmsd    = verify_with_solver( dsets, result, kase.noisflag, nchecked, share, crmsd );
      out() << QString( "  verify: the %1 largest solutes (%2% of the signal), refitted: RMSD %3 with the"
                        " Lamm-equation solver, %4 with the closed form%5\n\n" )
                  .arg( nchecked )
                  .arg( 100.0 * share, 0, 'f', 1 )
                  .arg( vrmsd, 0, 'e', 4 )
                  .arg( crmsd, 0, 'e', 4 )
                  .arg( vrmsd > 1.5 * crmsd ? QString( " -- the closed form does not hold: the run had not reached"
                                                       " equilibrium" )
                                            : QString() );
      jr["verify_rmsd"]        = vrmsd;
      jr["verify_closed_rmsd"] = crmsd;
      jr["verify_share"]       = share;
      out().flush();
   }

   jr["rmsd_spread"]       = spread;
   jr["datasets_over_cap"] = ds_over;
   jr["decoy_vbar"]        = kase.decoy_vbar;

   jr["rmsd_global"] = result.rmsd;
   jr["vbar_fit"]    = got_vbar;
   jr["vbar_true"]   = exp_vbar;
   jr["s_fit"]       = got_s;
   jr["s_true"]      = exp_s;
   jr["contrast"]    = result.contrast;
   jr["vbar_resol"]  = result.vbar_resol;
   jr["components"]  = result.model.components.size();
   jr["nscaliter"]   = result.nscaliter;
   jr["threads_requested"] = kase.threads;
   jr["threads_used"]      = result.nthreads;
   jr["nsimul"]      = result.nsimul;
   jr["msecs"]       = (double) result.msecs;
   jr["pass"]        = pass;

   if ( !out_path.isEmpty() )
   {
      QFile f( out_path );
      if ( f.open( QIODevice::WriteOnly | QIODevice::Text ) )
      {
         f.write( QJsonDocument( jr ).toJson() );
         f.close();
      }
   }

   if ( !model_path.isEmpty() )
   {
      result.model.write( model_path );
   }

   if ( !curves_path.isEmpty() )
   {
      double crmsd = 0.0;
      if ( write_curves( curves_path, dsets, kase, result, crmsd ) )
      {
         out() << QString( "  curves written to %1 (refitted RMSD %2)\n" ).arg( curves_path ).arg( crmsd, 0, 'e', 4 );
         out().flush();
      }
   }

   qDeleteAll( dsets );
   return pass ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Case generation
// ---------------------------------------------------------------------------
namespace
{

struct GenSpecies
{
      double s20w, ff0, vbar20, conc;
};

// One cell of a series.  The noise fields default to a negative sentinel
// meaning "whatever the case declares"; a case that varies noise from cell to
// cell sets them explicitly.  rmsd_max likewise: 0 means "use the case-wide
// cap", which is the only sensible reading when every cell is alike.
struct GenDataSet
{
      double d2o_pct;
      double loading;
      double rnoise   = -1.0;
      double tinoise  = -1.0;
      double rinoise  = -1.0;
      double rmsd_max = 0.0;
      int    rpm      = 0; // 0: SIM_RPM
      int    hours    = 0; // 0: SIM_HOURS
};

struct GenCase
{
      QString             name;
      QString             description;
      QVector<GenSpecies> species;
      QVector<GenDataSet> datasets;
      double              rnoise          = 0.0;
      double              tinoise         = 0.0;
      double              rinoise         = 0.0;
      int                 noisflag        = 0;
      double              vbar_tol        = 0.02;
      double              s_tol           = 0.5e-13;
      double              rmsd_max        = 1.0e-3; // noise-free cases fit to ~1e-4
      double              rmsd_spread_max = 20.0;
      bool                must_refuse     = false;
      bool                ignore_contrast = false;
      int                 v_res           = 11;
      double              v_min = 0.60, v_max = 0.85;
      double              edit_margin        = 0.02;
      double              edit_bottom_margin = 0.10;
      int                 grid_reps          = 1;
};

// The v-bar to plant in the analyte and solution records in place of the
// truth, so that a 2DSA cross-check does not start from the answer.
//
// Half a tenth of a mL/g away from the truth, in whichever direction leaves
// more room inside the range a protein plausibly occupies.  Deterministic, so
// a regenerated case tree is comparable with the last one.
double decoy_vbar_for( double truth )
{
   const double lo    = 0.62;
   const double hi    = 0.83;
   const double delta = 0.05;

   const double down = truth - delta;
   const double up   = truth + delta;

   // Prefer whichever candidate sits further from the edges of the range.
   const double down_room = qMin( down - lo, hi - down );
   const double up_room   = qMin( up - lo, hi - up );

   return ( down_room >= up_room ) ? down : up;
}

// Concentration-weighted v-bar of a case's species: the truth a fit is asked
// to recover, and what the decoy is measured against.
double truth_vbar_of( const QVector<GenSpecies>& species )
{
   double num = 0.0;
   double den = 0.0;

   for ( const GenSpecies& sp : species )
   {
      num += sp.vbar20 * sp.conc;
      den += sp.conc;
   }

   return ( den > 0.0 ) ? ( num / den ) : TYPICAL_VBAR;
}

// The 40 cases the harness runs.  Between them they cover every axis the
// request named: species differing in s, in D, in vbar and in combination;
// mixtures sharing a vbar while differing in s and D; unequal cell loadings;
// random, time-invariant and radially-invariant noise; D2O series of two to
// five isotope concentrations, including two that are deliberately too weak
// to determine vbar at all; and mixtures whose species change places in s
// across the series.
QVector<GenCase> build_cases()
{
   QVector<GenCase> cases;

   const GenSpecies A{ 4.0e-13, 1.5, 0.7300, 0.50 }; // the reference
   const GenSpecies B{ 6.5e-13, 1.5, 0.7300, 0.30 }; // same vbar, larger s
   const GenSpecies C{ 4.0e-13, 2.5, 0.7300, 0.30 }; // same s, smaller D
   const GenSpecies D{ 4.0e-13, 1.5, 0.6900, 0.40 }; // lower vbar
   const GenSpecies E{ 6.5e-13, 2.0, 0.7700, 0.40 }; // higher vbar, other s
   const GenSpecies F{ 2.0e-13, 1.3, 0.7300, 0.35 }; // small, same vbar
   const GenSpecies G{ 8.0e-13, 1.8, 0.7500, 0.25 }; // large, other vbar

   auto             add = [&]( const GenCase& c ) { cases << c; };

   // ---- 1-5: one species, growing series and loading spread --------------
   {
      GenCase c;
      c.name        = "case01_single_2buffers";
      c.description = "One species, H2O and 100% D2O, equal loading, no noise";
      c.species << A;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case02_single_3buffers";
      c.description = "One species, 0/50/100% D2O, equal loading, no noise";
      c.species << A;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case03_single_5buffers";
      c.description = "One species, 0/25/50/75/100% D2O, equal loading";
      c.species << A;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                 << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case04_single_unequal_loading";
      c.description = "One species, 0/50/100% D2O, loadings 1.0 / 0.7 / 1.4";
      c.species << A;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 0.7 } << GenDataSet{ 100.0, 1.4 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case05_single_extreme_loading";
      c.description = "One species, loadings 1.0 / 0.4 / 2.0 -- five-fold spread";
      c.species << A;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 0.4 } << GenDataSet{ 100.0, 2.0 };
      add( c );
   }

   // ---- 6-12: mixtures ---------------------------------------------------
   {
      GenCase c;
      c.name        = "case06_two_species_same_vbar_diff_s";
      c.description = "Two species, same vbar, s of 4.0 and 6.5 S";
      c.species << A << B;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case07_two_species_same_vbar_diff_s_and_D";
      c.description = "Two species, same vbar, different s and different D";
      c.species << A << GenSpecies{ 6.5e-13, 2.2, 0.7300, 0.30 };
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case08_two_species_same_s_diff_D";
      c.description = "Two species, same vbar and s, f/f0 of 1.5 and 2.5";
      c.species << A << C;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      c.vbar_tol = 0.025;
      add( c );
   }

   {
      GenCase c;
      c.name        = "case09_two_species_diff_vbar_same_s";
      c.description = "Two species, same s, vbar of 0.73 and 0.69";
      c.species << A << D;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      c.vbar_tol = 0.025;
      add( c );
   }

   {
      GenCase c;
      c.name        = "case10_two_species_diff_vbar_diff_s";
      c.description = "Two species differing in s, D and vbar";
      c.species << A << E;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      c.vbar_tol = 0.03;
      add( c );
   }

   {
      GenCase c;
      c.name        = "case11_three_species_same_vbar";
      c.description = "Three species sharing vbar, s of 2.0, 4.0 and 6.5 S";
      c.species << F << A << B;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case12_three_species_mixed_vbar_5buffers";
      c.description = "Three species, two sharing vbar, one differing, 5 buffers";
      c.species << A << B << G;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                 << GenDataSet{ 100.0, 1.0 };
      c.vbar_tol = 0.025;
      add( c );
   }

   // ---- 13-19: noise -----------------------------------------------------
   {
      GenCase c;
      c.name        = "case13_random_noise_low";
      c.description = "One species, random noise 0.001 OD";
      c.species << A;
      c.rnoise   = 0.001;
      c.rmsd_max = 0.0018; // random noise is not fitted; it stays in
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case14_random_noise_high";
      c.description = "One species, random noise 0.005 OD";
      c.species << A;
      c.rnoise   = 0.005;
      c.vbar_tol = 0.02;
      c.rmsd_max = 0.0090; // random noise is not fitted; it stays in
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case15_ti_noise";
      c.description = "One species, time-invariant noise 0.005 OD, TI fitted";
      c.species << A;
      c.tinoise         = 0.005;
      c.noisflag        = 1;
      c.vbar_tol        = 0.02;
      // TI is fitted, so it must be removed: the residual should fall far
      // below the injected level, and no data set may be left out.
      c.rmsd_max        = 0.0010;
      c.rmsd_spread_max = 5.0;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case16_ri_noise";
      c.description = "One species, radially-invariant noise 0.005 OD, RI fitted";
      c.species << A;
      c.rinoise         = 0.005;
      c.noisflag        = 2;
      c.vbar_tol        = 0.02;
      c.rmsd_max        = 0.0010;
      c.rmsd_spread_max = 5.0;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case17_all_noise";
      c.description = "One species, random + TI + RI noise, both noises fitted";
      c.species << A;
      c.rnoise          = 0.002;
      c.tinoise         = 0.005;
      c.rinoise         = 0.005;
      c.noisflag        = 3;
      c.vbar_tol        = 0.02;
      // TI and RI removed, random left: the residual should sit near 0.002.
      c.rmsd_max        = 0.0040;
      c.rmsd_spread_max = 5.0;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case18_mixture_all_noise";
      c.description = "Two species, random + TI + RI noise, both noises fitted";
      c.species << A << B;
      c.rnoise          = 0.002;
      c.tinoise         = 0.005;
      c.rinoise         = 0.005;
      c.noisflag        = 3;
      c.vbar_tol        = 0.02;
      c.rmsd_max        = 0.0040;
      c.rmsd_spread_max = 5.0;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case19_mixture_noise_unequal_loading_5buffers";
      c.description = "Three species, noise, unequal loadings, five isotope steps";
      c.species << F << A << B;
      c.rnoise          = 0.002;
      c.tinoise         = 0.003;
      c.noisflag        = 1;
      c.vbar_tol        = 0.02;
      c.rmsd_max        = 0.0040;
      c.rmsd_spread_max = 5.0;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 0.8 } << GenDataSet{ 50.0, 1.3 } << GenDataSet{ 75.0, 0.6 }
                 << GenDataSet{ 100.0, 1.5 };
      add( c );
   }

   // ---- 20-24: edges and gates -------------------------------------------
   {
      GenCase c;
      c.name        = "case20_weak_contrast_refused";
      c.description = "Only 0 and 15% D2O: contrast below the gate, must refuse";
      c.species << A;
      c.must_refuse = true;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 15.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case21_single_dataset_refused";
      c.description = "One data set: vbar is not determined, must refuse";
      c.species << A;
      c.must_refuse = true;
      c.datasets << GenDataSet{ 0.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case22_low_vbar";
      c.description = "One species at vbar 0.65, the low edge of the grid";
      c.species << GenSpecies{ 4.0e-13, 1.5, 0.6500, 0.50 };
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case23_high_vbar";
      c.description = "One species at vbar 0.80, the high edge of the grid";
      c.species << GenSpecies{ 4.0e-13, 1.5, 0.8000, 0.50 };
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case24_wide_s_range_mixed_vbar";
      c.description = "1.5 S and 8.0 S species with different vbar, 5 buffers";
      c.species << GenSpecies{ 1.5e-13, 1.2, 0.7100, 0.40 } << G;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                 << GenDataSet{ 100.0, 1.0 };
      c.vbar_tol = 0.03;
      add( c );
   }

   // ---- 25-28: unequal random noise, cell by cell ------------------------
   //
   // A series is not one experiment: each cell is loaded, scanned and read
   // separately, so its noise is its own.  These cases inject a different
   // random level in every cell, which makes the aggregate RMSD cap and the
   // spread check meaningless -- a spread of 6x is now the correct answer.
   // Each data set therefore carries its own cap, and the check is that every
   // cell's residual lands near the noise *it* was given.
   {
      GenCase c;
      c.name        = "case25_random_noise_per_dataset";
      c.description = "One species, random noise 0.001 / 0.003 / 0.006 OD by cell";
      c.species << A;
      c.rmsd_spread_max = 0.0; // a spread is expected here, not a defect
      c.rmsd_max        = 0.0; // per data set instead
      c.datasets << GenDataSet{ 0.0, 1.0, 0.001, -1.0, -1.0, 0.0020 } << GenDataSet{ 50.0, 1.0, 0.003, -1.0, -1.0, 0.0050 }
                 << GenDataSet{ 100.0, 1.0, 0.006, -1.0, -1.0, 0.0100 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case26_random_noise_per_dataset_mixture";
      c.description = "Two species, 0/30/70% D2O, random noise 0.004 / 0.001 / 0.002";
      c.species << A << B;
      c.rmsd_spread_max = 0.0;
      c.rmsd_max        = 0.0;
      c.vbar_tol        = 0.025;
      // The noisiest cell first, so a fit that quietly weights by data set
      // order rather than by residual shows up.
      c.datasets << GenDataSet{ 0.0, 1.0, 0.004, -1.0, -1.0, 0.0070 } << GenDataSet{ 30.0, 1.0, 0.001, -1.0, -1.0, 0.0020 }
                 << GenDataSet{ 70.0, 1.0, 0.002, -1.0, -1.0, 0.0040 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case27_noise_per_dataset_unequal_loading";
      c.description = "Random noise and loading both varying by cell, 0/20/45/80% D2O";
      c.species << A;
      c.rmsd_spread_max = 0.0;
      c.rmsd_max        = 0.0;
      // Noise and loading pull the amplitude factors in opposite directions:
      // the noisiest cell is also the most weakly loaded, which is the worst
      // case for a scale loop that confuses signal with residual.
      c.datasets << GenDataSet{ 0.0, 1.0, 0.001, -1.0, -1.0, 0.0020 } << GenDataSet{ 20.0, 0.5, 0.005, -1.0, -1.0, 0.0090 }
                 << GenDataSet{ 45.0, 1.6, 0.002, -1.0, -1.0, 0.0040 } << GenDataSet{ 80.0, 0.8, 0.003, -1.0, -1.0, 0.0055 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case28_ti_ri_per_dataset";
      c.description = "TI and RI noise at different levels per cell, 0/35/65/100% D2O";
      c.species << A << B;
      c.noisflag        = 3;
      c.vbar_tol        = 0.025;
      c.rmsd_spread_max = 0.0;
      c.rmsd_max        = 0.0;
      // TI and RI are fitted, so each cell's residual should fall to its own
      // random level whatever systematic noise it was given.  A shared noise
      // vector cannot satisfy four different pairs at once.
      c.datasets << GenDataSet{ 0.0, 1.0, 0.001, 0.002, 0.001, 0.0025 } << GenDataSet{ 35.0, 1.0, 0.002, 0.008, 0.004, 0.0045 }
                 << GenDataSet{ 65.0, 1.0, 0.001, 0.005, 0.002, 0.0025 } << GenDataSet{ 100.0, 1.0, 0.003, 0.003, 0.006, 0.0060 };
      add( c );
   }

   // ---- 29-31: isotope series that are not 0/50/100 ----------------------
   //
   // Every earlier multi-buffer case steps the D2O evenly and ends at 100%.
   // Real series rarely do either: stock runs out, a cell is lost, and the
   // top of the range costs the most H/D exchange.  These check that the
   // fit's contrast gate and its v-bar recovery depend on the densities
   // themselves and not on the series being tidy.
   {
      GenCase c;
      c.name        = "case29_series_0_30_50";
      c.description = "0/30/50% D2O -- no pure-D2O cell, uneven spacing";
      c.species << A;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 30.0, 1.0 } << GenDataSet{ 50.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case30_series_0_20_40_60";
      c.description = "0/20/40/60% D2O, mixture -- half the usual contrast";
      c.species << A << D;
      c.vbar_tol = 0.03;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 20.0, 1.0 } << GenDataSet{ 40.0, 1.0 } << GenDataSet{ 60.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case31_series_uneven_5_steps";
      c.description = "10/35/55/70/95% D2O -- unevenly spaced, no H2O cell";
      c.species << A << G;
      c.vbar_tol = 0.03;
      c.datasets << GenDataSet{ 10.0, 1.0 } << GenDataSet{ 35.0, 0.9 } << GenDataSet{ 55.0, 1.2 } << GenDataSet{ 70.0, 1.0 }
                 << GenDataSet{ 95.0, 0.7 };
      add( c );
   }

   // ---- 32-33: the solver's own process-wide state -----------------------
   //
   // Every earlier case fits one subgrid, so each level is a single task and
   // runs on one thread however many are requested; and every earlier series
   // runs all its cells at one speed for one time.  Both hid defects in the
   // headless build's Lamm-equation solver (design document section 3D).
   {
      GenCase c;
      c.name        = "case32_subgrids_threaded";
      c.description = "Eight interleaved subgrids, four threads requested";
      c.species << A << B;
      c.grid_reps = 2;
      c.vbar_tol  = 0.02;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case33_series_different_run_timing";
      c.description = "Each cell at its own speed and run time: 45k/5h, 50k/4h, 38k/6h";
      c.species << A;
      // Simulated within one second of each other, these cells used to share
      // the first cell's timestate and be fitted at the wrong speed.
      c.datasets << GenDataSet{ 0.0, 1.0, -1.0, -1.0, -1.0, 0.0, 45000, 5 }
                 << GenDataSet{ 50.0, 1.0, -1.0, -1.0, -1.0, 0.0, 50000, 4 }
                 << GenDataSet{ 100.0, 1.0, -1.0, -1.0, -1.0, 0.0, 38000, 6 };
      add( c );
   }

   // ---- 34-40: species that change places across the series -------------
   //
   // A species of high vbar loses more of its buoyancy to D2O than one of low
   // vbar, so two species can swap order in s from one cell to the next.  A
   // method that pairs the cells by boundary fraction, as us_density_match
   // does, then compares one species' s in H2O with the other's in D2O; a
   // global fit has no pairing to get wrong.  Unlike the cases above, every
   // s, f/f0 and vbar here lies off both the 3DSA grid and the default 2DSA
   // grid, so neither method is helped by a truth that sits on a grid point.
   //
   // That costs the noise-free cases their usual residual: a truth between
   // grid points fits to the grid's own floor, 6e-4 to 1e-3 here rather than
   // the ~1e-4 of the cases above, so their cap is raised to match.
   const GenSpecies P{ 4.20e-13, 1.35, 0.6600, 0.50 }; // slower in H2O, faster in D2O
   const GenSpecies Q{ 5.10e-13, 1.60, 0.8050, 0.40 }; // the other way round
   const double     OFF_GRID_RMSD_MAX = 1.5e-3;

   {
      GenCase c;
      c.name        = "case34_crossing_pair";
      c.description = "4.2 S / vbar 0.66 and 5.1 S / vbar 0.805: they swap order between 50 and 100% D2O";
      c.species << P << Q;
      c.vbar_tol = 0.02;
      c.rmsd_max = OFF_GRID_RMSD_MAX;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case35_crossing_pair_5buffers";
      c.description = "The crossing pair over five isotope steps; the swap falls inside the series";
      c.species << P << Q;
      c.vbar_tol = 0.02;
      c.rmsd_max = OFF_GRID_RMSD_MAX;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                 << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case36_crossing_coincide_at_50";
      c.description = "Crossing pair tuned to sediment together in the 50% D2O cell";
      // 4.82 S at vbar 0.805 has the same s as 4.2 S at vbar 0.66 in 50% D2O
      c.species << P << GenSpecies{ 4.82e-13, 1.60, 0.8050, 0.40 };
      c.vbar_tol = 0.02;
      c.rmsd_max = OFF_GRID_RMSD_MAX;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case37_crossing_pair_all_noise";
      c.description = "The crossing pair with random + TI + RI noise, both noises fitted";
      c.species << P << Q;
      c.rnoise          = 0.002;
      c.tinoise         = 0.005;
      c.rinoise         = 0.005;
      c.noisflag        = 3;
      c.vbar_tol        = 0.025;
      c.rmsd_max        = 0.0040;
      c.rmsd_spread_max = 5.0;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case38_crossing_pair_low_signal";
      c.description = "The crossing pair at a fifth of the loading (0.10 / 0.08 OD), random noise 0.003";
      c.species << GenSpecies{ P.s20w, P.ff0, P.vbar20, 0.10 } << GenSpecies{ Q.s20w, Q.ff0, Q.vbar20, 0.08 };
      c.rnoise   = 0.003;
      c.vbar_tol = 0.03;
      c.rmsd_max = 0.0050; // random noise is not fitted; it stays in
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case39_vbar_ladder_5species";
      c.description = "Five species, vbar rising with s from 0.645 to 0.82; the top two swap in D2O";
      // vbar rising with s compresses the distribution in D2O: by 100% the
      // fastest species has fallen behind the second fastest and level with
      // the middle one.
      c.species << GenSpecies{ 3.10e-13, 1.30, 0.6450, 0.15 } << GenSpecies{ 4.05e-13, 1.40, 0.6900, 0.15 }
                << GenSpecies{ 5.05e-13, 1.55, 0.7350, 0.15 } << GenSpecies{ 6.05e-13, 1.65, 0.7800, 0.15 }
                << GenSpecies{ 7.10e-13, 1.80, 0.8200, 0.15 };
      c.vbar_tol = 0.02;
      c.rmsd_max = OFF_GRID_RMSD_MAX;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 25.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 75.0, 1.0 }
                 << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   {
      GenCase c;
      c.name        = "case40_anticorrelated_pair_control";
      c.description = "Control: vbar 0.79 at 4.2 S and 0.66 at 6.3 S -- the order holds and the gap widens";
      c.species << GenSpecies{ 4.20e-13, 1.35, 0.7900, 0.50 } << GenSpecies{ 6.30e-13, 1.60, 0.6600, 0.40 };
      c.vbar_tol = 0.02;
      c.rmsd_max = OFF_GRID_RMSD_MAX;
      c.datasets << GenDataSet{ 0.0, 1.0 } << GenDataSet{ 50.0, 1.0 } << GenDataSet{ 100.0, 1.0 };
      add( c );
   }

   return cases;
}

// Write the simulation-parameter XML one data set needs.
//
// The noise is the data set's own where it declares any, and the case's
// otherwise, so a series can carry a different noise level in every cell.
bool write_simparams( const QString& path, const GenCase& c, const GenDataSet& ds )
{
   US_SimulationParameters sp;
   sp.simpoints         = SIM_POINTS;
   sp.radial_resolution = SIM_DELTA_R;
   sp.meniscus          = SIM_MENISCUS;
   sp.bottom            = SIM_BOTTOM;
   sp.bottom_position   = SIM_BOTTOM;
   sp.temperature       = SIM_TEMP;
   sp.rnoise            = ( ds.rnoise >= 0.0 ) ? ds.rnoise : c.rnoise;
   sp.tinoise           = ( ds.tinoise >= 0.0 ) ? ds.tinoise : c.tinoise;
   sp.rinoise           = ( ds.rinoise >= 0.0 ) ? ds.rinoise : c.rinoise;
   sp.baseline          = 0.0;
   sp.sim               = true;

   // A cell may run at its own speed and for its own time; a real series is
   // rarely one run, so the cells' speed profiles need not agree.
   const int rpm   = ( ds.rpm > 0 ) ? ds.rpm : SIM_RPM;
   const int hours = ( ds.hours > 0 ) ? ds.hours : SIM_HOURS;

   sp.speed_step[0].duration_hours    = hours;
   sp.speed_step[0].duration_minutes  = 0.0;
   sp.speed_step[0].delay_hours       = 0;
   sp.speed_step[0].delay_minutes     = 4.0;
   sp.speed_step[0].scans             = SIM_SCANS;
   sp.speed_step[0].rotorspeed        = rpm;
   sp.speed_step[0].acceleration      = 400;
   sp.speed_step[0].acceleration_flag = true;
   sp.speed_step[0].set_speed         = rpm;
   sp.speed_step[0].avg_speed         = (double) rpm;

   return ( sp.save_simparms( path ) == 0 );
}

// Write the buffer XML one data set needs.
bool write_buffer( const QString& path, double d2o_pct )
{
   US_Buffer b;
   b.GUID            = US_Util::new_guid();
   b.description     = QString( "3DSA harness, %1% D2O" ).arg( d2o_pct, 0, 'f', 0 );
   b.pH              = 7.0;
   b.density         = d2o_density( d2o_pct );
   b.viscosity       = d2o_viscosity( d2o_pct );
   b.compressibility = 0.0;
   // Not "manual": the declared density and viscosity are 20 C values and
   // both the simulator and the fit apply the same temperature correction to
   // them.  The two sides must agree on this flag or their corrections drift.
   b.manual          = false;

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
bool write_model( const QString& path, const GenCase& c, const GenDataSet& ds, const QStringList& analyte_guids )
{
   US_Model m;
   m.description = c.name;
   m.modelGUID   = US_Util::new_guid();
   m.analysis    = US_Model::MANUAL;

   for ( int ii = 0; ii < c.species.size(); ii++ )
   {
      const GenSpecies&             sp = c.species[ii];

      US_Model::SimulationComponent comp;
      comp.s                    = sp.s20w;
      comp.f_f0                 = sp.ff0;
      comp.vbar20               = sp.vbar20;
      comp.D                    = 0.0;
      comp.mw                   = 0.0;
      comp.f                    = 0.0;
      comp.signal_concentration = sp.conc * ds.loading;
      comp.name                 = QString::asprintf( "%s SP%02d", qPrintable( c.name ), ii + 1 );

      // One analyte GUID per species, the same in every cell of the series.
      //
      // us_astfem_sim registers an analyte per component and names it from
      // this GUID in the solution it writes.  Left empty, every species of
      // every case would register as an anonymous analyte and nothing could
      // be traced back to the run that produced it -- which is what a 2DSA
      // cross-check has to do to find the record whose vbar it should use.
      comp.analyteGUID = ( ii < analyte_guids.size() ) ? analyte_guids[ii] : US_Util::new_guid();

      // Fill in D, mw and f from s, f/f0 and vbar; leave them in 20W space.
      if ( !US_Model::calc_coefficients( comp ) )
      {
         return false;
      }

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
      if ( args[ii] == "--outdir" && ii + 1 < args.size() )
      {
         outdir = args[++ii];
      }
      else
      {
         err() << "unknown gen-cases option: " << args[ii] << "\n";
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

   const QVector<GenCase> cases = build_cases();
   QJsonArray             jcases;

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
         o["s20w"]   = sp.s20w;
         o["ff0"]    = sp.ff0;
         o["vbar20"] = sp.vbar20;
         o["conc"]   = sp.conc;
         jspecies << o;
      }

      // The nominal vbar the fit starts from: the concentration-weighted
      // truth would be cheating, so use the conventional protein value.
      const double nominal_vbar = TYPICAL_VBAR;

      // What the analyte and solution records get instead of the truth, so a
      // 2DSA cross-check has to work for its answer like an analyst would.
      const double truth_vbar = truth_vbar_of( c.species );
      const double decoy_vbar = decoy_vbar_for( truth_vbar );

      // The same species in every cell of the series, so the analyte the
      // simulator registers is one record shared by the whole run.
      QStringList  analyte_guids;
      for ( int ii = 0; ii < c.species.size(); ii++ )
      {
         analyte_guids << US_Util::new_guid();
      }

      for ( int ii = 0; ii < c.datasets.size(); ii++ )
      {
         const GenDataSet& ds    = c.datasets[ii];
         const QString     tag   = QString::asprintf( "ds%02d", ii );
         const QString     runid = c.name + "-" + tag;

         const QString     mpath = QString( "%1/%2_model.xml" ).arg( indir ).arg( tag );
         const QString     bpath = QString( "%1/%2_buffer.xml" ).arg( indir ).arg( tag );
         const QString     spath = QString( "%1/%2_simparams.xml" ).arg( indir ).arg( tag );
         const QString     ddir  = QString( "%1/data/%2" ).arg( outdir ).arg( runid );

         if ( !write_model( mpath, c, ds, analyte_guids ) )
         {
            err() << "cannot write " << mpath << "\n";
            err().flush();
            return 3;
         }
         if ( !write_buffer( bpath, ds.d2o_pct ) )
         {
            err() << "cannot write " << bpath << "\n";
            err().flush();
            return 3;
         }
         if ( !write_simparams( spath, c, ds ) )
         {
            err() << "cannot write " << spath << "\n";
            err().flush();
            return 3;
         }

         QDir().mkpath( ddir );

         QJsonObject step;
         step["runid"]              = runid;
         step["model"]              = mpath;
         step["buffer"]             = bpath;
         step["simparams"]          = spath;
         step["outdir"]             = ddir;
         // What the driver needs to rewrite the edit profile and the analyte
         // and solution records the simulator registers.
         step["meniscus"]           = SIM_MENISCUS;
         step["bottom"]             = SIM_BOTTOM;
         step["edit_margin"]        = c.edit_margin;
         step["edit_bottom_margin"] = c.edit_bottom_margin;
         step["decoy_vbar"]         = decoy_vbar;
         step["truth_vbar"]         = truth_vbar;
         jsteps << step;

         QJsonObject jd;
         // us_astfem_sim names the file <runid>.<type>.<cell>.<chan>.<wl>.auc
         jd["auc"]         = QString( "%1/%2.RA.1.A.280.auc" ).arg( ddir ).arg( runid );
         jd["simparams"]   = spath;
         jd["label"]       = tag + QString( " (%1% D2O)" ).arg( ds.d2o_pct, 0, 'f', 0 );
         jd["d2o_percent"] = ds.d2o_pct;
         jd["density"]     = d2o_density( ds.d2o_pct );
         jd["viscosity"]   = d2o_viscosity( ds.d2o_pct );
         jd["temperature"] = SIM_TEMP;
         jd["loading"]     = ds.loading;
         jd["vbar20"]      = nominal_vbar;
         jd["rpm"]         = ( ds.rpm > 0 ) ? ds.rpm : SIM_RPM;
         jd["hours"]       = ( ds.hours > 0 ) ? ds.hours : SIM_HOURS;
         jd["rnoise"]      = ( ds.rnoise >= 0.0 ) ? ds.rnoise : c.rnoise;
         jd["tinoise"]     = ( ds.tinoise >= 0.0 ) ? ds.tinoise : c.tinoise;
         jd["rinoise"]     = ( ds.rinoise >= 0.0 ) ? ds.rinoise : c.rinoise;
         jd["rmsd_max"]    = ds.rmsd_max;
         jd["run_dir"]     = ddir;
         jd["run_id"]      = runid;
         jdatasets << jd;
      }

      QJsonObject grid;
      grid["s_min"]     = 1.0e-13;
      grid["s_max"]     = 10.0e-13;
      grid["s_res"]     = 19; // step 0.5 S
      grid["k_min"]     = 1.0;
      grid["k_max"]     = 3.0;
      grid["k_res"]     = 9; // step 0.25
      grid["v_min"]     = c.v_min;
      grid["v_max"]     = c.v_max;
      grid["v_res"]     = c.v_res; // step 0.025
      grid["grid_reps"] = c.grid_reps;

      QJsonObject noise;
      noise["random"] = c.rnoise;
      noise["ti"]     = c.tinoise;
      noise["ri"]     = c.rinoise;

      QJsonObject fit;
      fit["threads"]            = 4;
      fit["noisflag"]           = c.noisflag;
      fit["fit_scales"]         = true;
      fit["edit_margin"]        = c.edit_margin;
      fit["edit_bottom_margin"] = c.edit_bottom_margin;
      fit["ignore_contrast"]    = c.ignore_contrast;
      fit["decoy_vbar"]         = decoy_vbar;

      QJsonObject expect;
      expect["vbar_tol"]        = c.vbar_tol;
      expect["s_tol"]           = c.s_tol;
      expect["must_refuse"]     = c.must_refuse;
      expect["rmsd_max"]        = c.rmsd_max;
      expect["rmsd_spread_max"] = c.rmsd_spread_max;

      QJsonObject jc;
      jc["name"]          = c.name;
      jc["description"]   = c.description;
      jc["truth_vbar"]    = truth_vbar;
      jc["decoy_vbar"]    = decoy_vbar;
      jc["analyte_guids"] = QJsonArray::fromStringList( analyte_guids );
      jc["species"]       = jspecies;
      jc["datasets"]      = jdatasets;
      jc["noise"]         = noise;
      jc["grid"]          = grid;
      jc["fit"]           = fit;
      jc["expect"]        = expect;

      const QString cpath = QString( "%1/cases/%2.json" ).arg( outdir ).arg( c.name );
      QFile         cf( cpath );
      if ( !cf.open( QIODevice::WriteOnly | QIODevice::Text ) )
      {
         err() << "cannot write " << cpath << "\n";
         err().flush();
         return 3;
      }
      cf.write( QJsonDocument( jc ).toJson() );
      cf.close();

      QJsonObject jman;
      jman["name"]        = c.name;
      jman["description"] = c.description;
      jman["config"]      = cpath;
      jman["sim_steps"]   = jsteps;
      jcases << jman;
   }

   QJsonObject manifest;
   manifest["cases"]     = jcases;
   manifest["generated"] = QDateTime::currentDateTime().toString( Qt::ISODate );

   const QString mpath = outdir + "/manifest.json";
   QFile         mf( mpath );
   if ( !mf.open( QIODevice::WriteOnly | QIODevice::Text ) )
   {
      err() << "cannot write " << mpath << "\n";
      err().flush();
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
int US_3dsaCli::run_simulate( const QStringList& args )
{
   QString  cfg_path;
   unsigned seed = 1;

   for ( int ii = 0; ii < args.size(); ii++ )
   {
      if ( args[ii] == "--config" && ii + 1 < args.size() )
      {
         cfg_path = args[++ii];
      }
      else if ( args[ii] == "--seed" && ii + 1 < args.size() )
      {
         seed = args[++ii].toUInt();
      }
      else
      {
         err() << "unknown simulate option: " << args[ii] << "\n";
         err().flush();
         return 2;
      }
   }

   Case    kase;
   QString emsg;

   if ( cfg_path.isEmpty() || !read_case( cfg_path, kase, emsg ) )
   {
      err() << "simulate needs --config <case.json>" << ( emsg.isEmpty() ? QString() : ": " + emsg ) << "\n";
      err().flush();
      return 2;
   }

   if ( kase.species.isEmpty() )
   {
      err() << cfg_path << ": no species to simulate\n";
      err().flush();
      return 2;
   }

   for ( int ii = 0; ii < kase.datasets.size(); ii++ )
   {
      const DataSetSpec&      spec = kase.datasets[ii];
      US_SimulationParameters sp;

      if ( spec.simparams.isEmpty() || sp.load_simparms( spec.simparams ) != 0 || spec.auc.isEmpty() )
      {
         err() << "data set " << ii << ": simulate needs readable \"simparams\" and an \"auc\" to write\n";
         err().flush();
         return 3;
      }

      US_Buffer buffer;
      buffer.density   = spec.density;
      buffer.viscosity = spec.viscosity;
      buffer.manual    = false;

      if ( !spec.buffer.isEmpty() && !buffer.readFromDisk( spec.buffer ) )
      {
         err() << "data set " << ii << ": cannot read buffer " << spec.buffer << "\n";
         err().flush();
         return 3;
      }

      if ( !spec.buffer.isEmpty() )
      {
         buffer.density   = spec.has_density ? spec.density : buffer.density;
         buffer.viscosity = spec.has_viscosity ? spec.viscosity : buffer.viscosity;
      }

      const bool fvm = ( sp.meshType == US_SimulationParameters::ASTFVM );

      if ( US_FvmGradients::forms_gradient( buffer ) && !fvm )
      {
         err() << "data set " << ii << ": its buffer forms a gradient, which only the ASTFVM simulates\n";
         err().flush();
         return 3;
      }

      // The grid: the whole cell, and scans spread over the speed step after
      // the rotor reaches speed, omega-squared-t integrated over the
      // acceleration
      US_SimulationParameters::SpeedProfile& step     = sp.speed_step[0];
      const double                           dr       = ( sp.radial_resolution > 0.0 ) ? sp.radial_resolution : 0.001;
      const double                           duration = step.duration_hours * 3600.0 + step.duration_minutes * 60.0;
      const int                              nscans   = qMax( 1, step.scans );
      const double                           rpm      = step.rotorspeed;
      const double                           w2       = sq( rpm * M_PI / 30.0 );
      const double       t_acc = ( step.acceleration_flag && step.acceleration > 0 ) ? rpm / step.acceleration : 0.0;

      US_DataIO::RawData sim;
      sim.type[0]     = 'R';
      sim.type[1]     = 'A';
      sim.cell        = ii + 1;
      sim.channel     = 'A';
      sim.description = kase.name + ": " + ( spec.label.isEmpty() ? QString::number( ii ) : spec.label );
      US_Util::uuid_parse( US_Util::new_guid(), (unsigned char*) sim.rawGUID );

      const int nrad = qRound( ( sp.bottom - sp.meniscus ) / dr ) + 1;
      for ( int jj = 0; jj < nrad; jj++ )
      {
         sim.xvalues << sp.meniscus + jj * dr;
      }

      for ( int kk = 0; kk < nscans; kk++ )
      {
         US_DataIO::Scan sc;
         sc.temperature = sp.temperature;
         sc.rpm         = rpm;
         sc.seconds     = qMax( t_acc + 1.0, duration * ( kk + 1 ) / nscans );
         sc.omega2t     = w2 * ( sc.seconds - t_acc ) + w2 * t_acc / 3.0;
         sc.wavelength  = 280.0;
         sc.plateau     = 0.0;
         sc.delta_r     = dr;
         sc.rvalues     = QVector<double>( nrad, 0.0 );
         sim.scanData << sc;
      }

      US_SimulationParameters::computeSpeedSteps( &sim.scanData, sp.speed_step );

      QTemporaryDir tdir;
      const QString tmst = tdir.filePath( "sim.time_state.tmst" );
      US_AstfemMath::writetimestate( tmst, sp, sim );
      sp.simSpeedsFromTimeState( tmst );

      // The truth model.  The ASTFVM corrects s and D where the solute is,
      // from the standard-space values; the ASTFEM wants them corrected to
      // the buffer.
      US_Model model;
      for ( const Species& spc : kase.species )
      {
         US_Model::SimulationComponent c;
         c.s                    = spc.s20w;
         c.f_f0                 = spc.ff0;
         c.vbar20               = spc.vbar20;
         c.D                    = 0.0;
         c.mw                   = 0.0;
         c.f                    = 0.0;
         c.signal_concentration = spc.conc * spec.loading;
         model.components << c;
      }
      model.update_coefficients();

      if ( fvm )
      {
         US_FvmGradients grads;
         const double    t_end = sim.scanData.last().seconds;

         if ( !grads.prepare( buffer, sp, US_FvmGradients::gradient_grid( sp, t_end, 100, qMin( 0.002, dr ) ), emsg ) )
         {
            err() << "data set " << ii << ": " << emsg << "\n";
            err().flush();
            return 3;
         }

         if ( grads.is_gradient() )
         {
            out() << QString( "  %1: gradient %2 to %3 g/mL\n" )
                        .arg( spec.auc )
                        .arg( grads.density_lo(), 0, 'f', 4 )
                        .arg( grads.density_hi(), 0, 'f', 4 );
         }

         US_LammAstfvm astfvm( model, sp );
         astfvm.set_buffer( buffer, grads.band_forming(), grads.cosed() );
         astfvm.calculate( sim );
      }
      else
      {
         for ( US_Model::SimulationComponent& c : model.components )
         {
            const US_Math2::SolutionData sd = corrections( c.vbar20, buffer.density, buffer.viscosity, sp.temperature );
            c.s /= sd.s20w_correction;
            c.D /= sd.D20w_correction;
         }

         US_Astfem_RSA astfem( model, sp );
         astfem.calculate( sim );
      }

      const double rnoise = ( spec.rnoise > 0.0 ) ? spec.rnoise : kase.rnoise;

      if ( rnoise > 0.0 )
      {
         std::mt19937               gen( seed + 7919u * ii );
         std::normal_distribution<> gauss( 0.0, rnoise );

         for ( US_DataIO::Scan& sc : sim.scanData )
         {
            for ( double& vv : sc.rvalues )
            {
               vv += gauss( gen );
            }
         }
      }

      QDir().mkpath( QFileInfo( spec.auc ).absolutePath() );
      const int rc = US_DataIO::writeRawData( spec.auc, sim );

      if ( rc != US_DataIO::OK )
      {
         err() << "cannot write " << spec.auc << ": " << US_DataIO::errorString( rc ) << "\n";
         err().flush();
         return 3;
      }

      out() << QString( "  wrote %1: %2 scans of %3 points, %4 to %5 s%6\n" )
                  .arg( spec.auc )
                  .arg( sim.scanData.size() )
                  .arg( sim.xvalues.size() )
                  .arg( sim.scanData.first().seconds, 0, 'f', 0 )
                  .arg( sim.scanData.last().seconds, 0, 'f', 0 )
                  .arg( rnoise > 0.0 ? QString( ", noise %1 OD" ).arg( rnoise ) : QString() );
      out().flush();
   }

   return 0;
}

// ---------------------------------------------------------------------------
// A gradient former's molar concentration from interference scans.  A fringe
// measures a change of refractive index, so each scan fixes the profile's
// shape but not its level; the level is the one that conserves the former's
// mass over the sector, at the loading concentration.  The scan nearest each
// end of the cell is extrapolated from the last 0.03 cm of good data.  A
// uniform scan at the loading concentration starts the run.
int US_3dsaCli::run_gradient_from_interference( const QStringList& args )
{
   QString ip_path;
   QString out_path;
   double  meniscus   = 0.0;
   double  bottom     = 0.0;
   double  loading    = 0.0;   // g/mL
   double  dndc       = 0.0;   // mL/g
   double  molar_mass = 0.0;   // g/mol
   double  path       = 1.2;   // cm
   double  wavelength = 660.0; // nm
   double  dr         = 0.005;
   double  top_margin = 0.012;
   double  bot_margin = 0.025;
   int     stride     = 1;
   double  to_men     = 0.0; // Geometry of the data set the gradient serves
   double  to_bot     = 0.0;

   for ( int ii = 0; ii + 1 < args.size(); ii += 2 )
   {
      const QString a = args[ii];
      const QString v = args[ii + 1];
      if ( a == "--ip" )
      {
         ip_path = v;
      }
      else if ( a == "--out" )
      {
         out_path = v;
      }
      else if ( a == "--meniscus" )
      {
         meniscus = v.toDouble();
      }
      else if ( a == "--bottom" )
      {
         bottom = v.toDouble();
      }
      else if ( a == "--loading" )
      {
         loading = v.toDouble();
      }
      else if ( a == "--dndc" )
      {
         dndc = v.toDouble();
      }
      else if ( a == "--molar-mass" )
      {
         molar_mass = v.toDouble();
      }
      else if ( a == "--path" )
      {
         path = v.toDouble();
      }
      else if ( a == "--wavelength" )
      {
         wavelength = v.toDouble();
      }
      else if ( a == "--stride" )
      {
         stride = qMax( 1, v.toInt() );
      }
      else if ( a == "--dr" )
      {
         dr = v.toDouble();
      }
      else if ( a == "--top-margin" )
      {
         top_margin = v.toDouble();
      }
      else if ( a == "--bottom-margin" )
      {
         bot_margin = v.toDouble();
      }
      else if ( a == "--to-meniscus" )
      {
         to_men = v.toDouble();
      }
      else if ( a == "--to-bottom" )
      {
         to_bot = v.toDouble();
      }
      else
      {
         err() << "unknown gradient-from-interference option: " << a << "\n";
         err().flush();
         return 2;
      }
   }

   if ( ip_path.isEmpty() || out_path.isEmpty() || bottom <= meniscus || loading <= 0.0 || dndc <= 0.0 || molar_mass <= 0.0 )
   {
      err() << "gradient-from-interference needs --ip, --out, --meniscus, --bottom, --loading, --dndc and --molar-mass\n";
      err().flush();
      return 2;
   }

   US_DataIO::RawData ip;
   if ( US_DataIO::readRawData( ip_path, ip ) != US_DataIO::OK || ip.scanData.isEmpty() )
   {
      err() << "cannot read " << ip_path << "\n";
      err().flush();
      return 3;
   }

   const double       fringe_to_conc = wavelength * 1.0e-7 / ( path * dndc ); // g/mL per fringe
   const int          ngrid          = qMax( 2, qRound( ( bottom - meniscus ) / dr ) + 1 );
   const double       step           = ( bottom - meniscus ) / ( ngrid - 1 );
   const double       lo             = meniscus + top_margin;
   const double       hi             = bottom - bot_margin;

   US_DataIO::RawData molar;
   molar.type[0]     = 'R';
   molar.type[1]     = 'A';
   molar.cell        = ip.cell;
   molar.channel     = ip.channel;
   molar.description = QString( "molar gradient former from %1" ).arg( QFileInfo( ip_path ).fileName() );
   US_Util::uuid_parse( US_Util::new_guid(), (unsigned char*) molar.rawGUID );
   // The interference optics and the data the gradient serves may place the
   // cell slightly differently: the gradient is mapped meniscus to meniscus
   // and bottom to bottom
   const bool      remap  = ( to_bot > to_men && to_men > 0.0 );
   const double    t_men  = remap ? to_men : meniscus;
   const double    t_step = remap ? ( to_bot - to_men ) / ( ngrid - 1 ) : step;
   QVector<double> ipx;
   for ( int kk = 0; kk < ngrid; kk++ )
   {
      ipx << meniscus + step * kk;
      molar.xvalues << t_men + t_step * kk;
   }

   // The start of the run: uniform
   US_DataIO::Scan start = ip.scanData.first();
   start.seconds         = 0.0;
   start.omega2t         = 0.0;
   start.rvalues         = QVector<double>( ngrid, loading / molar_mass * 1000.0 );
   molar.scanData << start;

   // Least-squares line through the data on [a, b]
   const auto line = [&]( const US_DataIO::Scan& sc, double a, double b, double& c0, double& c1 )
   {
      double sn = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
      for ( int jj = 0; jj < ip.xvalues.size(); jj++ )
      {
         const double xx = ip.xvalues[jj];
         if ( xx < a || xx > b )
         {
            continue;
         }
         sn += 1;
         sx += xx;
         sy += sc.rvalues[jj];
         sxx += xx * xx;
         sxy += xx * sc.rvalues[jj];
      }
      c1 = ( sn * sxy - sx * sy ) / qMax( 1.0e-30, sn * sxx - sx * sx );
      c0 = ( sy - c1 * sx ) / qMax( 1.0, sn );
   };

   for ( int ss = 0; ss < ip.scanData.size(); ss++ )
   {
      if ( ss % stride != 0 && ss != ip.scanData.size() - 1 )
      {
         continue;
      }

      const US_DataIO::Scan& sc = ip.scanData[ss];
      double                 a0, a1, b0, b1;
      line( sc, lo, lo + 0.03, a0, a1 );
      line( sc, hi - 0.03, hi, b0, b1 );

      QVector<double> jv( ngrid );
      for ( int kk = 0; kk < ngrid; kk++ )
      {
         const double rr = ipx[kk];
         if ( rr < lo )
         {
            jv[kk] = a0 + a1 * rr;
            continue;
         }
         if ( rr > hi )
         {
            jv[kk] = b0 + b1 * rr;
            continue;
         }
         double sum = 0.0;
         int    nn  = 0;
         for ( int jj = 0; jj < ip.xvalues.size(); jj++ )
         {
            if ( qAbs( ip.xvalues[jj] - rr ) <= 0.5 * step )
            {
               sum += sc.rvalues[jj];
               nn++;
            }
         }
         jv[kk] = ( nn > 0 ) ? sum / nn : ( rr < 0.5 * ( lo + hi ) ? a0 + a1 * rr : b0 + b1 * rr );
      }

      // Level from mass conservation over the sector
      double num = 0.0;
      double den = 0.0;
      for ( int kk = 1; kk < ngrid; kk++ )
      {
         const double r0 = molar.xvalues[kk - 1];
         const double r1 = molar.xvalues[kk];
         num += 0.5 * ( jv[kk - 1] * r0 + jv[kk] * r1 ) * t_step;
         den += 0.5 * ( r0 + r1 ) * t_step;
      }
      const double    jmean = num / den;

      US_DataIO::Scan out = sc;
      out.rvalues.resize( ngrid );
      for ( int kk = 0; kk < ngrid; kk++ )
      {
         const double conc = loading + ( jv[kk] - jmean ) * fringe_to_conc; // g/mL
         out.rvalues[kk]   = qMax( 0.0, conc ) / molar_mass * 1000.0;       // mol/L
      }
      out.delta_r = t_step;
      molar.scanData << out;
   }

   const int rc = US_DataIO::writeRawData( out_path, molar );
   if ( rc != US_DataIO::OK )
   {
      err() << "cannot write " << out_path << "\n";
      err().flush();
      return 3;
   }

   const US_DataIO::Scan& last = molar.scanData.last();
   out() << QString( "  wrote %1: %2 scans of %3 points; last scan %4 to %5 M (loading %6 M)\n" )
               .arg( out_path )
               .arg( molar.scanData.size() )
               .arg( ngrid )
               .arg( last.rvalues.first(), 0, 'f', 4 )
               .arg( last.rvalues.last(), 0, 'f', 4 )
               .arg( loading / molar_mass * 1000.0, 0, 'f', 4 );
   out().flush();
   return 0;
}

// ---------------------------------------------------------------------------
int US_3dsaCli::main( int argc, char** argv )
{
   QStringList args;
   for ( int ii = 1; ii < argc; ii++ )
   {
      args << QString::fromLocal8Bit( argv[ii] );
   }

   if ( args.isEmpty() || args[0] == "--help" || args[0] == "-h" )
   {
      usage();
      return args.isEmpty() ? 2 : 0;
   }

   const QString cmd = args.takeFirst();

   if ( cmd == "fit" )
   {
      return run_fit( args );
   }
   if ( cmd == "gen-cases" )
   {
      return run_gen_cases( args );
   }
   if ( cmd == "simulate" )
   {
      return run_simulate( args );
   }
   if ( cmd == "gradient-from-interference" )
   {
      return run_gradient_from_interference( args );
   }

   err() << "unknown command: " << cmd << "\n";
   err().flush();
   usage();
   return 2;
}

// The simulators write their progress through qDebug, some of it
// unconditionally; a headless driver keeps only the warnings, unless
// US3DSA_DEBUG is set.
void quiet_debug( QtMsgType type, const QMessageLogContext&, const QString& msg )
{
   if ( type == QtDebugMsg || type == QtInfoMsg )
   {
      return;
   }

   std::fprintf( stderr, "%s\n", msg.toLocal8Bit().constData() );
}

int main( int argc, char** argv )
{
   if ( qEnvironmentVariableIsEmpty( "US3DSA_DEBUG" ) )
   {
      qInstallMessageHandler( quiet_debug );
   }

   QCoreApplication app( argc, argv );
   QCoreApplication::setApplicationName( "us_3dsa_cli" );
   return US_3dsaCli::main( argc, argv );
}
