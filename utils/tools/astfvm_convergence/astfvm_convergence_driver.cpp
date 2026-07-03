//! \file astfvm_convergence_driver.cpp
//!
//! Minimal headless driver for the ASTFVM instrumentation & convergence
//! controls added to US_LammAstfvm (see the Chapter 4 implementation spec).
//! It builds a single-component myoglobin model (ideal, constant s/D case;
//! Cao et al. Table 1), runs US_LammAstfvm::calculate() once with a chosen
//! (N, steps-per-transit|fixed-dt, refine, mesh-speed, uniform) combination,
//! and writes the solution trace (trace_steps.csv / trace_nodes.csv) via the
//! new setSolutionTrace() API. Intended to be run in a loop, once per point
//! on the (N, dt) sweep, by an external shell/python wrapper (see the
//! companion us3utils post-processor in ../us3utils).
//!
//! This program does not touch any GUI or database; it only needs a
//! QCoreApplication event loop because US_LammAstfvm::calculate() calls
//! qApp->processEvents() and emits Qt signals while it runs.
//!
//! NOTE (open items, see spec Sec. 9 -- confirm before using for publication
//! figures):
//!   - meniscus/bottom default to 5.8/7.2 cm (values already used elsewhere
//!     in this repo's test fixtures for a standard 12mm 2-channel cell).
//!   - rotor speed defaults to 50000 rpm, a typical SV speed for a small
//!     (~17 kDa) globular protein such as myoglobin.
//!   - s, D, vbar20 default to the paper's myoglobin values.
//! All of the above are overridable on the command line.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QTextStream>

#include "us_model.h"
#include "us_simparms.h"
#include "us_dataIO.h"
#include "us_buffer.h"
#include "us_lamm_astfvm.h"

namespace {

// Build a one-component myoglobin model (ideal case: constant s, D).
US_Model buildMyoglobinModel( double s, double D, double vbar20, double conc )
{
   US_Model model;
   model.coSedSolute = -1;

   US_Model::SimulationComponent comp;
   comp.name                = "myoglobin";
   comp.s                   = s;         // sedimentation coefficient (Svedberg, s)
   comp.D                   = D;         // diffusion coefficient (cm^2/s)
   comp.vbar20              = vbar20;    // partial specific volume
   comp.mw                  = 17800.0;   // approx. myoglobin MW (Da)
   comp.f_f0                = 1.17;      // frictional ratio (paper, Table 1)
   comp.sigma                = 0.0;      // ideal: no concentration dependence
   comp.delta                = 0.0;
   comp.signal_concentration = conc;     // OD units
   comp.extinction            = 1.0;
   comp.shape                = US_Model::SPHERE;

   model.components.clear();
   model.components << comp;
   return model;
}

// Build simulation parameters for a single constant-speed run.
US_SimulationParameters buildSimParams( double rpm, double meniscus, double bottom,
                                        double total_seconds, int n_scans,
                                        int init_Nelem, bool band_forming )
{
   US_SimulationParameters sp;
   sp.meshType   = US_SimulationParameters::ASTFVM;
   sp.gridType   = US_SimulationParameters::MOVING;
   sp.simpoints  = init_Nelem;
   sp.meniscus   = meniscus;
   sp.bottom     = bottom;
   sp.bottom_position = bottom;
   sp.temperature = 20.0;
   sp.band_forming = band_forming;
   sp.band_volume  = band_forming ? 0.015 : 0.0; // mL, only used if band_forming
   sp.cp_pathlen   = 1.2;
   sp.cp_angle     = 2.5;
   sp.rotorcoeffs[0] = 0.0; // no rotor stretch for this synthetic run
   sp.rotorcoeffs[1] = 0.0;
   sp.radial_resolution = ( bottom - meniscus ) / 1000.0;

   // Single constant-speed step spanning the whole run.
   US_SimulationParameters::SpeedProfile step;
   step.duration_hours   = static_cast<int>( total_seconds / 3600.0 );
   step.duration_minutes = ( total_seconds - step.duration_hours * 3600.0 ) / 60.0;
   step.delay_minutes    = 0.0;
   step.delay_hours      = 0;
   step.time_first        = 0;
   step.time_last          = static_cast<int>( total_seconds );
   step.scans             = n_scans;
   step.rotorspeed        = static_cast<int>( rpm );
   step.avg_speed          = rpm;
   step.acceleration       = 0;
   step.acceleration_flag = false;
   sp.speed_step.clear();
   sp.speed_step << step;

   // Constant-speed synthetic timestate: one rpm/w2t sample per second, for
   // the whole run. US_LammAstfvm::calculate() only reads rpm_timestate (see
   // solve_component's rpm interpolation); w2t_timestate is populated for
   // completeness but not consulted by the solver.
   US_SimulationParameters::SimSpeedProf ssp;
   const int duration = static_cast<int>( total_seconds ) + 2;
   ssp.rotorspeed  = static_cast<int>( rpm );
   ssp.avg_speed   = rpm;
   ssp.acceleration = 0.0;
   ssp.time_b_accel = 0;
   ssp.time_e_accel = 0;
   ssp.time_f_scan  = 0;
   ssp.time_l_scan  = static_cast<int>( total_seconds );
   ssp.time_e_step  = duration;
   ssp.duration     = duration;
   ssp.w2t_b_accel  = 0.0;
   ssp.w2t_e_accel  = 0.0;
   const double w2 = ( rpm * M_PI / 30.0 ) * ( rpm * M_PI / 30.0 );
   ssp.w2t_e_step   = w2 * total_seconds;
   ssp.rpm_timestate.resize( duration );
   ssp.w2t_timestate.resize( duration );
   for ( int i = 0; i < duration; i++ )
   {
      ssp.rpm_timestate[i] = rpm;
      ssp.w2t_timestate[i] = w2 * static_cast<double>( i );
   }
   sp.sim_speed_prof.clear();
   sp.sim_speed_prof << ssp;

   return sp;
}

// Build the output radius grid and scan-time list that US_LammAstfvm fills.
US_DataIO::RawData buildRawData( double meniscus, double bottom, int npoints,
                                 double total_seconds, int n_scans, double rpm )
{
   US_DataIO::RawData rd;
   rd.type[0]   = 'R';
   rd.type[1]   = 'A';
   rd.cell      = 1;
   rd.channel   = 'A';
   rd.description = "astfvm_convergence_driver synthetic run";

   rd.xvalues.resize( npoints );
   const double dr = ( bottom - meniscus ) / static_cast<double>( npoints - 1 );
   for ( int i = 0; i < npoints; i++ )
   {
      rd.xvalues[i] = meniscus + dr * static_cast<double>( i );
   }

   rd.scanData.resize( n_scans );
   const double w2 = ( rpm * M_PI / 30.0 ) * ( rpm * M_PI / 30.0 );
   for ( int i = 0; i < n_scans; i++ )
   {
      US_DataIO::Scan& sc = rd.scanData[i];
      sc.temperature = 20.0;
      sc.rpm         = rpm;
      // evenly spaced scan times across the run, skipping t=0
      sc.seconds     = total_seconds * static_cast<double>( i + 1 )
                     / static_cast<double>( n_scans );
      sc.omega2t     = w2 * sc.seconds;
      sc.wavelength  = 280.0;
      sc.plateau     = 0.0;
      sc.delta_r     = dr;
      sc.rvalues.fill( 0.0, npoints );
   }
   return rd;
}

} // namespace

int main( int argc, char* argv[] )
{
   QCoreApplication app( argc, argv );
   QCoreApplication::setApplicationName( "astfvm_convergence_driver" );

   QCommandLineParser parser;
   parser.setApplicationDescription(
      "Run one US_LammAstfvm ASTFVM simulation at a controlled resolution "
      "and export its solution trace (mesh + solution + mass per step)." );
   parser.addHelpOption();

   QCommandLineOption optOutDir( "outdir", "Trace output directory.", "dir", "./astfvm_trace_out" );
   QCommandLineOption optTag( "tag", "Run tag (used in output filenames).", "tag", "run" );
   QCommandLineOption optN( "N", "Initial number of mesh elements.", "n", "100" );
   QCommandLineOption optSteps( "steps-per-transit", "Steps per sedimentation transit (dt ~ 1/k). "
                                "Ignored if --fixed-dt is given.", "k", "100" );
   QCommandLineOption optFixedDt( "fixed-dt", "Force an exact time step (s); overrides --steps-per-transit.", "dt", "0" );
   QCommandLineOption optRefine( "refine", "Adaptive mesh refinement: 1=on, 0=off.", "0|1", "1" );
   QCommandLineOption optMeshSpeed( "mesh-speed", "Mesh speed factor: 1=moving, 0=fixed.", "0|1", "1" );
   QCommandLineOption optUniform( "uniform", "Keep initial mesh exactly uniform (skip setup refine).", "0|1", "0" );
   QCommandLineOption optErrTol( "err-tol", "Mesh refinement error tolerance.", "tol", "1e-5" );
   QCommandLineOption optBand( "band-forming", "Simulate a sharp overlay band instead of a full column.", "0|1", "0" );
   QCommandLineOption optRpm( "rpm", "Rotor speed (rpm).", "rpm", "50000" );
   QCommandLineOption optMeniscus( "meniscus", "Meniscus radius (cm).", "cm", "5.8" );
   QCommandLineOption optBottom( "bottom", "Bottom radius (cm).", "cm", "7.2" );
   QCommandLineOption optS( "s", "Sedimentation coefficient (Svedberg).", "s", "2.13e-13" );
   QCommandLineOption optD( "D", "Diffusion coefficient (cm^2/s).", "D", "1.08e-6" );
   QCommandLineOption optVbar( "vbar", "Partial specific volume (mL/g).", "vbar", "0.741" );
   QCommandLineOption optNPoints( "npoints", "Number of output radial points.", "n", "500" );
   QCommandLineOption optNScans( "nscans", "Number of output scans.", "n", "50" );
   QCommandLineOption optRunHours( "run-hours", "Total simulated run duration (hours).", "h", "3.0" );
   QCommandLineOption optStride( "trace-stride", "Emit a trace record every Nth calculated step.", "n", "1" );

   parser.addOption( optOutDir );
   parser.addOption( optTag );
   parser.addOption( optN );
   parser.addOption( optSteps );
   parser.addOption( optFixedDt );
   parser.addOption( optRefine );
   parser.addOption( optMeshSpeed );
   parser.addOption( optUniform );
   parser.addOption( optErrTol );
   parser.addOption( optBand );
   parser.addOption( optRpm );
   parser.addOption( optMeniscus );
   parser.addOption( optBottom );
   parser.addOption( optS );
   parser.addOption( optD );
   parser.addOption( optVbar );
   parser.addOption( optNPoints );
   parser.addOption( optNScans );
   parser.addOption( optRunHours );
   parser.addOption( optStride );
   parser.process( app );

   const QString outDir   = parser.value( optOutDir );
   const QString tag       = parser.value( optTag );
   const int    initN      = parser.value( optN ).toInt();
   const int    stepsPerT  = parser.value( optSteps ).toInt();
   const double fixedDt     = parser.value( optFixedDt ).toDouble();
   const bool   refineOn    = parser.value( optRefine ).toInt() != 0;
   const double meshSpeed   = parser.value( optMeshSpeed ).toDouble();
   const bool   uniform     = parser.value( optUniform ).toInt() != 0;
   const double errTol       = parser.value( optErrTol ).toDouble();
   const bool   bandForming = parser.value( optBand ).toInt() != 0;
   const double rpm          = parser.value( optRpm ).toDouble();
   const double meniscus     = parser.value( optMeniscus ).toDouble();
   const double bottom       = parser.value( optBottom ).toDouble();
   const double s_val         = parser.value( optS ).toDouble();
   const double D_val         = parser.value( optD ).toDouble();
   const double vbar          = parser.value( optVbar ).toDouble();
   const int    npoints       = parser.value( optNPoints ).toInt();
   const int    nscans        = parser.value( optNScans ).toInt();
   const double runHours      = parser.value( optRunHours ).toDouble();
   const int    traceStride  = parser.value( optStride ).toInt();

   const double totalSeconds = runHours * 3600.0;

   QDir dir;
   if ( !dir.exists( outDir ) )
   {
      dir.mkpath( outDir );
   }

   US_Model model = buildMyoglobinModel( s_val, D_val, vbar,
                                        bandForming ? 3100.0 : 1.0 );
   US_SimulationParameters simparams = buildSimParams(
      rpm, meniscus, bottom, totalSeconds, nscans, initN, bandForming );
   US_DataIO::RawData rawData = buildRawData(
      meniscus, bottom, npoints, totalSeconds, nscans, rpm );

   US_LammAstfvm solver( model, simparams );

   US_Buffer buffer; // defaults to water at 20C (density/viscosity only)
   solver.set_buffer( buffer );

   solver.setMovieFlag( false );
   solver.setInitElements( initN );
   if ( fixedDt > 0.0 )
   {
      solver.setFixedDt( fixedDt );
   }
   else
   {
      solver.setStepsPerTransit( stepsPerT );
   }
   solver.setUniformMesh( uniform );
   solver.setErrorTolerance( errTol );
   solver.SetMeshRefineOpt( refineOn ? 1 : 0 );
   solver.SetMeshSpeedFactor( meshSpeed );
   solver.setSolutionTrace( true, outDir, tag );
   solver.setTraceStride( traceStride );

   QTextStream out( stdout );
   out << "astfvm_convergence_driver: tag=" << tag
       << " N=" << initN
       << " steps_per_transit=" << stepsPerT
       << " fixed_dt=" << fixedDt
       << " refine=" << refineOn
       << " mesh_speed=" << meshSpeed
       << " uniform=" << uniform
       << " err_tol=" << errTol
       << " band_forming=" << bandForming
       << Qt::endl;

   const int rc = solver.calculate( rawData );
   if ( rc != 0 )
   {
      out << "calculate() returned non-zero: " << rc << Qt::endl;
      return rc;
   }

   out << "done; trace written under " << outDir
       << " with tag \"" << tag << "\"" << Qt::endl;
   return 0;
}
