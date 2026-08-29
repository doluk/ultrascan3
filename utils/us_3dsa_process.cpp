//! \file us_3dsa_process.cpp
#include "us_3dsa_process.h"

#include "us_astfem_math.h"
#include "us_math2.h"
#include "us_memory.h"
#include "us_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()
#endif

// ---------------------------------------------------------------------------
// Parameters and Result
// ---------------------------------------------------------------------------

US_3dsaProcess::Parameters::Parameters()
{
   x_min           = 1.0e-13;
   x_max           = 10.0e-13;
   x_res           = 64;
   y_min           = 1.0;
   y_max           = 4.0;
   y_res           = 64;
   z_min           = 0.60;
   z_max           = 0.85;
   z_res           = 16;
   grid_reps       = 8;
   s_mask          = mask_s_k_v();
   nthreads        = 1;
   noisflag        = 0;
   max_tsols       = 200;
   alpha           = 0.0;
   fit_scales      = true;
   scale_iters     = 12;
   scale_toler     = 1.0e-4;
   ignore_contrast = false;
}

US_3dsaProcess::Result::Result()
{
   variance        = 0.0;
   rmsd            = 0.0;
   contrast        = 0.0;
   vbar_resol      = 0.0;
   ngrid           = 0;
   nsubgrids       = 0;
   ndepths         = 0;
   ntasks          = 0;
   nsimul          = 0;
   nscaliter       = 0;
   msecs           = 0;
}

// ---------------------------------------------------------------------------

US_3dsaProcess::US_3dsaProcess( QList< US_SolveSim::DataSet* >& dsets,
                                QObject* parent )
   : QObject( parent ), dsets( dsets )
{
   dbg_level    = US_Settings::us_debug();
   errMsg       = QString( "" );
   tasks_done   = 0;
   tasks_expect = 0;
   abort_flag.storeRelaxed( 0 );
}

int US_3dsaProcess::mask_s_k_v()
{
   return ( US_Solute::ATTR_S << 6 ) | ( US_Solute::ATTR_K << 3 )
          | US_Solute::ATTR_V;
}

int US_3dsaProcess::mask_s_d_v()
{
   return ( US_Solute::ATTR_S << 6 ) | ( US_Solute::ATTR_D << 3 )
          | US_Solute::ATTR_V;
}

void US_3dsaProcess::abort_fit()
{
   abort_flag.storeRelaxed( 1 );
}

// ---------------------------------------------------------------------------
// Per-thread data sets
//
// calc_residuals() writes into dset->simparams (loading a timestate, setting
// meniscus and bottom), so threads cannot share data sets.  Each thread gets
// its own deep copy, made once per fit rather than once per task.
// ---------------------------------------------------------------------------
void US_3dsaProcess::make_thread_datasets( int nthreads )
{
   const int ndsets = dsets.size();

   thr_store.clear();
   thr_dsets.clear();
   thr_store.resize( nthreads );
   thr_dsets.resize( nthreads );

   for ( int tt = 0; tt < nthreads; tt++ )
   {
      thr_store[ tt ].resize( ndsets );

      for ( int dd = 0; dd < ndsets; dd++ )
      {
         thr_store[ tt ][ dd ] = *dsets[ dd ];
         thr_dsets[ tt ] << &thr_store[ tt ][ dd ];
      }
   }
}

// ---------------------------------------------------------------------------
// One NNLS solve
// ---------------------------------------------------------------------------
void US_3dsaProcess::run_task( int thrx, const Task& task, TaskResult& res,
                               const Parameters& parms )
{
   res.ok     = false;
   res.nsimul = 0;

   if ( task.isolutes.isEmpty() )
   {  // Nothing to do, but not an error: an empty subgrid contributes nothing
      res.ok = true;
      return;
   }

   QList< US_SolveSim::DataSet* >& tdsets = thr_dsets[ thrx ];

   US_SolveSim::Simulation sim_vals;
   sim_vals.solutes    = task.isolutes;
   sim_vals.noisflag   = task.noisflag;
   sim_vals.alpha      = parms.alpha;
   sim_vals.dbg_level  = 0;
   sim_vals.dbg_timing = false;
   sim_vals.scales     = task.scales;

   // Each solute costs one Lamm-equation solve per data set.
   res.nsimul          = task.isolutes.size() * tdsets.size();

   // US_SolveSimMDS, not US_SolveSim: 3DSA is global over a buoyancy-contrast
   // series, and each run in that series carries its own TI and RI noise.
   // US_SolveSim computes noise from the first data set alone.
   US_SolveSimMDS solver( tdsets, thrx + 1, false );

   if ( abort_flag.loadRelaxed() != 0 )
      return;

   solver.calc_residuals( 0, tdsets.size(), sim_vals );

   if ( abort_flag.loadRelaxed() != 0 )
      return;

   // calc_residuals resizes the solute vector to qMax( survivors, 1 ), so a
   // fit that kept nothing still returns one entry, with zero concentration.
   res.csolutes = sim_vals.solutes;

   if ( res.csolutes.size() == 1  &&  res.csolutes[ 0 ].c <= 0.0 )
      res.csolutes.clear();

   res.variance  = sim_vals.variance;
   res.variances = sim_vals.variances;

   if ( task.keep_sim )
   {
      res.ti_noise = sim_vals.ti_noise;
      res.ri_noise = sim_vals.ri_noise;
      res.sim_data = sim_vals.sim_data;
   }

   res.ok = true;
}

// ---------------------------------------------------------------------------
// One level of independent tasks
//
// Every task at a level is independent, so the level is a plain parallel map
// followed by a barrier.  Results stay indexed by task, which keeps the fit
// deterministic regardless of thread count or scheduling.
// ---------------------------------------------------------------------------
bool US_3dsaProcess::run_level( const QVector< Task >& tasks,
                                QVector< TaskResult >& results,
                                const Parameters& parms )
{
   const int ntasks   = tasks.size();
   results.resize( ntasks );

   const int nthreads = qBound( 1, parms.nthreads, ntasks );

   if ( nthreads == 1 )
   {
      for ( int ii = 0; ii < ntasks; ii++ )
      {
         if ( abort_flag.loadRelaxed() != 0 )  return false;

         run_task( 0, tasks[ ii ], results[ ii ], parms );
         tasks_done++;
         emit progress_update( tasks_done, tasks_expect );
      }
   }

   else
   {
      std::atomic< int >         next( 0 );
      std::vector< std::thread > pool;
      pool.reserve( nthreads );

      for ( int tt = 0; tt < nthreads; tt++ )
      {
         pool.emplace_back( [ & , tt ]()
         {
            for ( ;; )
            {
               const int ii = next.fetch_add( 1 );
               if ( ii >= ntasks )                    break;
               if ( abort_flag.loadRelaxed() != 0 )   break;

               run_task( tt, tasks[ ii ], results[ ii ], parms );
            }
         } );
      }

      for ( std::thread& th : pool )
         th.join();

      tasks_done += ntasks;
      emit progress_update( tasks_done, tasks_expect );
   }

   if ( abort_flag.loadRelaxed() != 0 )
   {
      errMsg = tr( "The fit was aborted." );
      return false;
   }

   for ( int ii = 0; ii < ntasks; ii++ )
   {
      if ( ! results[ ii ].ok )
      {
         errMsg = results[ ii ].error.isEmpty()
                  ? tr( "Task %1 failed." ).arg( ii )
                  : results[ ii ].error;
         return false;
      }
   }

   return true;
}

// ---------------------------------------------------------------------------
// Split a solute set into tasks
// ---------------------------------------------------------------------------
QVector< US_3dsaProcess::Task > US_3dsaProcess::repartition(
      const QVector< US_Solute >& solutes, int max_tsols,
      const QVector< double >& scales )
{
   QVector< Task > tasks;
   const int nsol = solutes.size();

   if ( nsol < 1 )  return tasks;

   const int chunk  = qMax( 1, max_tsols );
   const int ntask  = ( nsol + chunk - 1 ) / chunk;
   // Even chunks beat one short tail task: the level waits for its slowest.
   const int size   = ( nsol + ntask - 1 ) / ntask;

   for ( int ii = 0; ii < nsol; ii += size )
   {
      Task task;
      task.noisflag = 0;          // noise is fitted only on the final pass
      task.keep_sim = false;
      task.scales   = scales;
      task.isolutes = solutes.mid( ii, qMin( size, nsol - ii ) );
      tasks << task;
   }

   return tasks;
}

// ---------------------------------------------------------------------------
// Per-data-set amplitude factors
//
// Given the concentrations, the amplitude that minimises the residual for
// data set e has the closed form  <b_e, s_e> / <s_e, s_e>, where s_e is the
// simulated block.  The simulation already carries the previous iteration's
// scale, so the update is multiplicative.  The gauge is fixed by holding the
// first data set at 1, leaving the concentrations to absorb the overall
// level.
// ---------------------------------------------------------------------------
bool US_3dsaProcess::update_scales( US_DataIO::RawData& sim_data,
                                    QVector< double >& scales,
                                    double& max_change )
{
   const int ndsets = dsets.size();
   max_change       = 0.0;

   if ( scales.size() != ndsets )
      scales = QVector< double >( ndsets, 1.0 );

   QVector< double > factor( ndsets, 1.0 );
   int scnx = 0;

   for ( int dd = 0; dd < ndsets; dd++ )
   {
      US_DataIO::EditedData* edata = &dsets[ dd ]->run_data;
      const int nscans  = edata->scanCount();
      const int npoints = edata->pointCount();

      double bs = 0.0;
      double ss = 0.0;

      for ( int sc = 0; sc < nscans; sc++, scnx++ )
      {
         for ( int rr = 0; rr < npoints; rr++ )
         {
            const double sval = sim_data.value( scnx, rr );
            bs += edata->value( sc, rr ) * sval;
            ss += sval * sval;
         }
      }

      if ( ss <= 0.0 )
      {  // No signal simulated for this set: leave its scale alone
         factor[ dd ] = 1.0;
         continue;
      }

      factor[ dd ] = bs / ss;
   }

   // Hold data set 0 at unity so the scales and concentrations are not
   // jointly degenerate.
   const double gauge = ( factor[ 0 ] > 0.0 ) ? factor[ 0 ] : 1.0;

   for ( int dd = 0; dd < ndsets; dd++ )
   {
      const double relative = factor[ dd ] / gauge;

      if ( ! ( relative > 0.0 ) )  continue;   // reject non-finite or <= 0

      const double updated = scales[ dd ] * relative;
      max_change = qMax( max_change,
                         qAbs( updated - scales[ dd ] )
                         / qMax( 1.0e-30, qAbs( scales[ dd ] ) ) );
      scales[ dd ] = updated;
   }

   return true;
}

// ---------------------------------------------------------------------------
// Accelerating the amplitude sequence
//
// Alternating between the NNLS and the closed-form amplitudes converges, but
// only geometrically: each step is partial because the NNLS re-absorbs some
// of the correction by spreading concentration over neighbouring grid points.
// Measured on a three-buffer series the ratio is a near-constant 0.79 per
// iteration, which would need about 26 passes over the grid to settle -- far
// too many when a pass is the whole fit.
//
// A geometric sequence has a known limit, so take it.  Working in log space
// (the amplitudes are multiplicative), three successive iterates give
//
//     r = (L2 - L1) / (L1 - L0),   L* = L2 + (L2 - L1) * r / (1 - r)
//
// which is exact when the convergence really is geometric and is skipped
// when it is not.  Steffensen-style: the caller restarts the history after
// an extrapolation so the next estimate is built from fresh iterates.
// ---------------------------------------------------------------------------
bool US_3dsaProcess::extrapolate_scales( QVector< double >& scales,
                                         const QVector< double >& prev1,
                                         const QVector< double >& prev2 )
{
   const int ndsets = scales.size();

   if ( prev1.size() != ndsets  ||  prev2.size() != ndsets )
      return false;

   bool applied = false;

   for ( int dd = 0; dd < ndsets; dd++ )
   {
      if ( scales[ dd ] <= 0.0  ||  prev1[ dd ] <= 0.0  ||
           prev2[ dd ] <= 0.0 )                              continue;

      const double l2 = log( scales[ dd ] );
      const double l1 = log( prev1 [ dd ] );
      const double l0 = log( prev2 [ dd ] );
      const double d1 = l1 - l0;
      const double d2 = l2 - l1;

      if ( qAbs( d1 ) < 1.0e-12 )                            continue;

      const double ratio = d2 / d1;

      // Only extrapolate a sequence that is actually contracting toward a
      // limit, and not one so close to 1 that the sum is meaningless.
      if ( ratio <= 0.0  ||  ratio >= 0.98 )                  continue;

      const double limit = l2 + d2 * ratio / ( 1.0 - ratio );

      if ( ! std::isfinite( limit ) )                         continue;

      // Never let one step move the amplitude by more than an order of
      // magnitude; that would mean the geometric assumption was wrong.
      if ( qAbs( limit - l2 ) > log( 10.0 ) )                 continue;

      scales[ dd ] = exp( limit );
      applied      = true;
   }

   return applied;
}

// ---------------------------------------------------------------------------
// Final model
//
// The grid is in standard (20W) space and so is the model written out, which
// is what a model file holds.  Conversion to experimental space happens
// inside calc_residuals, per data set, and is not baked into the result.
// ---------------------------------------------------------------------------
bool US_3dsaProcess::build_model( const QVector< US_Solute >& solutes,
                                  const Parameters& parms, US_Model& model )
{
   const int attr_x = ( parms.s_mask >> 6 ) & 7;
   const int attr_y = ( parms.s_mask >> 3 ) & 7;
   const int attr_z =   parms.s_mask        & 7;

   model.components.clear();
   model.analysis   = US_Model::THREEDSA;
   model.global     = ( dsets.size() > 1 ) ? US_Model::GLOBAL
                                           : US_Model::NONE;

   auto put = [ & ]( US_Model::SimulationComponent& comp,
                     const US_Solute& sol, int a_type )
   {
      switch ( a_type )
      {
         case US_Solute::ATTR_S:  comp.s      = sol.s;  break;
         case US_Solute::ATTR_K:  comp.f_f0   = sol.k;  break;
         case US_Solute::ATTR_V:  comp.vbar20 = sol.v;  break;
         case US_Solute::ATTR_W:  comp.mw     = sol.d;  break;
         case US_Solute::ATTR_D:  comp.D      = sol.d;  break;
         default:                 comp.f      = sol.d;  break;
      }
   };

   for ( int cc = 0; cc < solutes.size(); cc++ )
   {
      US_Model::SimulationComponent comp;
      comp.s      = 0.0;
      comp.D      = 0.0;
      comp.mw     = 0.0;
      comp.f      = 0.0;
      comp.f_f0   = 0.0;
      comp.vbar20 = 0.0;

      put( comp, solutes[ cc ], attr_x );
      put( comp, solutes[ cc ], attr_y );
      put( comp, solutes[ cc ], attr_z );

      comp.signal_concentration = solutes[ cc ].c;

      if ( ! US_Model::calc_coefficients( comp ) )
      {
         errMsg = tr( "Could not complete the coefficients of solute %1." )
                  .arg( cc );
         return false;
      }

      comp.name = QString::asprintf( "SC%04d", cc + 1 );
      model.components << comp;
   }

   return true;
}

// ---------------------------------------------------------------------------
// The fit
// ---------------------------------------------------------------------------
bool US_3dsaProcess::fit( const Parameters& parms, Result& result )
{
   QElapsedTimer timer;
   timer.start();

   errMsg     = QString( "" );
   result     = Result();
   tasks_done = 0;
   abort_flag.storeRelaxed( 0 );

   const int ndsets = dsets.size();

   if ( ndsets < 1 )
   {
      errMsg = tr( "No data sets were supplied." );
      return false;
   }

   // ---- the axis mask has to be representable -----------------------------
   QString mask_err;
   if ( ! US_Solute::validate_mask( parms.s_mask, mask_err ) )
   {
      errMsg = mask_err;
      return false;
   }

   // ---- and the series has to be able to determine vbar -------------------
   const bool fits_vbar = ( ( ( parms.s_mask >> 6 ) & 7 ) == US_Solute::ATTR_V
                         || ( ( parms.s_mask >> 3 ) & 7 ) == US_Solute::ATTR_V
                         || (   parms.s_mask        & 7 ) == US_Solute::ATTR_V );

   const double vbar_mid = ( parms.z_min + parms.z_max ) * 0.5;
   QString contrast_msg;
   result.contrast   = US_SolveSim::buoyancy_contrast( dsets, vbar_mid,
                                                       contrast_msg );
   result.vbar_resol = US_SolveSim::vbar_resolution( result.contrast, 0.01 );

   if ( fits_vbar  &&  result.contrast < US_SolveSim::VBAR_CONTRAST_REFUSE
                  &&  ! parms.ignore_contrast )
   {
      errMsg = tr( "This series cannot determine vbar.  %1" )
               .arg( contrast_msg );
      return false;
   }

   emit message_update( contrast_msg );

   // ---- the grid ----------------------------------------------------------
   QList< QVector< US_Solute > > subgrids;
   const int reps = US_Solute::init_solutes_3d(
                       parms.x_min, parms.x_max, parms.x_res,
                       parms.y_min, parms.y_max, parms.y_res,
                       parms.z_min, parms.z_max, parms.z_res,
                       parms.grid_reps, parms.s_mask, subgrids );

   if ( reps < 1  ||  subgrids.isEmpty() )
   {
      errMsg = tr( "The grid could not be generated." );
      return false;
   }

   result.nsubgrids = subgrids.size();
   for ( const QVector< US_Solute >& sv : subgrids )
      result.ngrid += sv.size();

   if ( result.ngrid < 1 )
   {
      errMsg = tr( "The grid contains no usable points." );
      return false;
   }

   // ---- worker set-up -----------------------------------------------------
   const int nthreads = qMax( 1, parms.nthreads );
   make_thread_datasets( nthreads );

   // Every data set fits vbar per solute, so the buffer corrections must be
   // recomputed from each solute's own vbar rather than taken from the set.
   for ( int tt = 0; tt < nthreads; tt++ )
      for ( int dd = 0; dd < ndsets; dd++ )
      {
         thr_store[ tt ][ dd ].solute_type = parms.s_mask;
         thr_store[ tt ][ dd ].fit_vbar    = fits_vbar;
      }

   // Depth 0 plus a rough allowance for the levels above it.
   tasks_expect = subgrids.size() * 2;

   emit message_update( tr( "Fitting %1 grid points in %2 subgrids across"
                            " %3 data sets" )
                        .arg( result.ngrid ).arg( subgrids.size() )
                        .arg( ndsets ) );

   // ---- the fit, once per scale-factor iteration --------------------------
   QVector< double > scales( ndsets, 1.0 );
   QVector< double > scales_prev1;
   QVector< double > scales_prev2;
   QVector< US_Solute > survivors;
   TaskResult final_res;
   const int  max_scale_iters = parms.fit_scales
                                ? qMax( 1, parms.scale_iters ) : 1;

   for ( int siter = 0; siter < max_scale_iters; siter++ )
   {
      result.nscaliter = siter + 1;

      // ---- depth 0: one task per subgrid ----------------------------------
      QVector< Task > tasks;
      for ( const QVector< US_Solute >& sv : subgrids )
      {
         Task task;
         task.isolutes = sv;
         task.scales   = scales;
         task.noisflag = 0;         // noise is fitted only on the final pass
         task.keep_sim = false;
         tasks << task;
      }

      // Depth levels merge the survivors of the level below and re-fit them,
      // narrowing the solute set until one task holds all of it.  NNLS keeps
      // only a handful of solutes per task, so this converges in a few
      // levels; the cap is a backstop, not the expected exit.
      const int max_depth = 24;
      int       depth     = 0;
      QVector< TaskResult > results;

      for ( ;; )
      {
         if ( ! run_level( tasks, results, parms ) )
            return false;

         result.ntasks += tasks.size();
         survivors.clear();

         for ( const TaskResult& tres : results )
         {
            result.nsimul += tres.nsimul;
            survivors     += tres.csolutes;
         }

         // One task at this level means its survivors are already the whole
         // solution; another level would only repeat it.
         if ( tasks.size() == 1 )  break;

         if ( survivors.isEmpty() )
         {
            errMsg = tr( "No solutes survived the fit at depth %1." )
                     .arg( depth );
            return false;
         }

         const int prev_ntasks = tasks.size();
         depth++;
         tasks = repartition( survivors, parms.max_tsols, scales );

         if ( tasks.size() >= prev_ntasks  ||  depth >= max_depth )
         {  // Not narrowing: finish in a single task rather than spin
            tasks = repartition( survivors, survivors.size(), scales );
         }
      }

      result.ndepths = depth + 1;

      // ---- final pass: all survivors, with noise, keeping the simulation --
      Task final_task;
      final_task.isolutes = survivors;
      final_task.noisflag = parms.noisflag;
      final_task.keep_sim = true;

      final_task.scales   = scales;

      QVector< Task >       final_tasks { final_task };
      QVector< TaskResult > final_results;

      if ( ! run_level( final_tasks, final_results, parms ) )
         return false;

      final_res      = final_results[ 0 ];
      result.ntasks += 1;
      result.nsimul += final_res.nsimul;
      survivors      = final_res.csolutes;

      if ( survivors.isEmpty() )
      {
         errMsg = tr( "The final fit retained no solutes." );
         return false;
      }

      if ( ! parms.fit_scales  ||  ndsets < 2 )
         break;

      double change = 0.0;
      scales_prev2  = scales_prev1;
      scales_prev1  = scales;
      update_scales( final_res.sim_data, scales, change );

      if ( extrapolate_scales( scales, scales_prev1, scales_prev2 ) )
      {  // Restart the history: the next estimate needs fresh iterates
         scales_prev1.clear();
         scales_prev2.clear();
         change = 1.0;         // always verify an extrapolation by re-fitting
      }

      QString sctxt;
      for ( double sc : scales )
         sctxt += QString( " %1" ).arg( sc, 0, 'f', 5 );

      emit message_update( tr( "Scale iteration %1: largest change %2,"
                               " scales%3, rmsd %4, components %5" )
                           .arg( siter + 1 ).arg( change, 0, 'e', 2 )
                           .arg( sctxt )
                           .arg( sqrt( qMax( 0.0, final_res.variance ) ),
                                 0, 'e', 3 )
                           .arg( survivors.size() ) );

      if ( change < parms.scale_toler )
         break;
   }

   // ---- results -----------------------------------------------------------
   if ( ! build_model( survivors, parms, result.model ) )
      return false;

   result.scales    = scales;
   result.ti_noise  = final_res.ti_noise;
   result.ri_noise  = final_res.ri_noise;
   result.variance  = final_res.variance;
   result.variances = final_res.variances;
   result.rmsd      = sqrt( qMax( 0.0, final_res.variance ) );
   result.msecs     = timer.elapsed();

   result.report    = tr( "3DSA: %1 grid points, %2 subgrids, %3 depth levels,"
                          " %4 NNLS solves, %5 Lamm solves, %6 components,"
                          " RMSD %7, contrast %8 (mL/g)^-1, vbar resolution"
                          " %9 mL/g, %10 s" )
                      .arg( result.ngrid ).arg( result.nsubgrids )
                      .arg( result.ndepths ).arg( result.ntasks )
                      .arg( result.nsimul )
                      .arg( result.model.components.size() )
                      .arg( result.rmsd, 0, 'e', 4 )
                      .arg( result.contrast, 0, 'f', 3 )
                      .arg( result.vbar_resol, 0, 'f', 4 )
                      .arg( result.msecs / 1000.0, 0, 'f', 1 );

   emit message_update( result.report );

   return true;
}
