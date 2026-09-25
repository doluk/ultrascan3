//! \file us_optimality_2d.cpp
#include "us_optimality_2d.h"
#include "us_settings.h"
#include "us_math2.h"

#ifndef DbgLv
#define DbgLv(a) if(dbg_level>=a)qDebug()
#endif

// Flag if two solutes are the same grid point
static bool same_point( const US_Solute& s1, const US_Solute& s2 )
{
   return ( s1.s == s2.s  &&  s1.k == s2.k  &&  s1.v == s2.v );
}

// Project a scan-major column (nscans x npoints) onto the complement of the
// noise space:  remove what time-invariant noise (a constant per radius) and
// radially-invariant noise (a constant per scan) could absorb.
static void project_noise( const double* aa, double* pa, int nscans,
                           int npoints, int noisflag )
{
   int ntot       = nscans * npoints;

   for ( int ii = 0; ii < ntot; ii++ )
      pa[ ii ]       = aa[ ii ];

   if ( ( noisflag & 1 ) != 0 )
   {  // Remove the mean over scans at each radius
      for ( int rr = 0; rr < npoints; rr++ )
      {
         double mean    = 0.0;
         for ( int ss = 0; ss < nscans; ss++ )
            mean          += pa[ ss * npoints + rr ];
         mean          /= (double)nscans;
         for ( int ss = 0; ss < nscans; ss++ )
            pa[ ss * npoints + rr ] -= mean;
      }
   }

   if ( ( noisflag & 2 ) != 0 )
   {  // Remove the mean over radii of each scan
      for ( int ss = 0; ss < nscans; ss++ )
      {
         double* ps     = pa + ss * npoints;
         double mean    = 0.0;
         for ( int rr = 0; rr < npoints; rr++ )
            mean          += ps[ rr ];
         mean          /= (double)npoints;
         for ( int rr = 0; rr < npoints; rr++ )
            ps[ rr ]      -= mean;
      }
   }
}

// Worker constructor:  copy the data set in the calling (main) thread
US_OptimalityWorker::US_OptimalityWorker( SS_DATASET* dset,
      const QList< QVector< US_Solute > >& batches,
      const QVector< double >& resid, int noisflag, int thrn,
      const QVector< double >& qbasis )
   : QThread(), batches( batches ), resid( resid ), qbasis( qbasis ),
     noisflag( noisflag ), thrn( thrn )
{
   dset_wk        = *dset;
   abort          = false;
   refit          = false;
}

// Make this a refit worker with the given batch ids
void US_OptimalityWorker::set_refit( const QList< int >& ids )
{
   refit          = true;
   bids           = ids;
}

// Simulate solutes (sorted) without noise; return the A matrix and B vector
bool US_OptimalityWorker::simulate( const QVector< US_Solute >& sols,
                                    QVector< double >& amat,
                                    QVector< double >& bvals )
{
   if ( abort )
      return false;

   QList< SS_DATASET* > dsets;
   dsets << &dset_wk;
   US_SolveSim solvesim( dsets, thrn, false );
   US_SolveSim::Simulation sim_vals;
   sim_vals.noisflag   = 0;     // Columns only; noise handled here
   sim_vals.dbg_level  = 0;
   sim_vals.dbg_timing = false;
   sim_vals.alpha      = 0.0;
   sim_vals.solutes    = sols;

   solvesim.calc_residuals( 0, 1, sim_vals, false, &amat, &bvals );
   return true;
}

// Simulate each batch of solutes. Without a residual, return the simulated
// columns; with one, compute each point's projected gradient and gain.
void US_OptimalityWorker::run()
{
   int nscans     = dset_wk.run_data.scanCount();
   int npoints    = dset_wk.run_data.pointCount();

   for ( int bb = 0; bb < batches.size(); bb++ )
   {
      if ( abort )
         break;

      if ( refit )
      {  // Fit the batch as the 2DSA does and record its sum of squares
         QList< SS_DATASET* > dsets;
         dsets << &dset_wk;
         US_SolveSim solvesim( dsets, thrn, false );
         US_SolveSim::Simulation sim_vals;
         sim_vals.noisflag   = noisflag;
         sim_vals.dbg_level  = 0;
         sim_vals.dbg_timing = false;
         sim_vals.alpha      = 0.0;
         sim_vals.solutes    = batches[ bb ];
         solvesim.calc_residuals( 0, 1, sim_vals );
         double ssq     = 0.0;
         for ( int ss = 0; ss < sim_vals.residuals.scanCount(); ss++ )
            for ( int rr = 0; rr < sim_vals.residuals.pointCount(); rr++ )
               ssq           += sq( sim_vals.residuals.value( ss, rr ) );
         refits << qMakePair( bids.value( bb, bb ), ssq );
         emit batch_done( 0 );
         continue;
      }

      // Columns come back for the input solutes in sorted order, less any
      // cut for lack of signal. If any were cut, simulate one at a time.
      QVector< US_Solute > bsols = batches[ bb ];
      std::sort( bsols.begin(), bsols.end() );
      QVector< US_Solute > usols;
      QVector< double >    amat;
      QVector< double >    bvals;

      if ( ! simulate( bsols, amat, bvals ) )
         break;

      int ntot       = bvals.size();
      int ncols      = ( ntot > 0 ) ? ( amat.size() / ntot ) : 0;

      if ( ncols == bsols.size() )
         usols          = bsols;

      else
      {  // Some columns were cut:  identify them one solute at a time
         amat.clear();

         for ( int jj = 0; jj < bsols.size(); jj++ )
         {
            QVector< US_Solute > one;
            QVector< double >    acol;
            QVector< double >    bone;
            one << bsols[ jj ];

            if ( ! simulate( one, acol, bone ) )
               break;

            if ( ! bone.isEmpty()  &&  acol.size() == bone.size() )
            {
               usols         << bsols[ jj ];
               amat          += acol;
               ntot           = bone.size();
               bvals          = bone;
            }
         }

         ncols          = usols.size();
      }

      if ( resid.isEmpty() )
      {  // Return simulated columns
         bvec           = bvals;
         csols         += usols;
         acols         += amat;
      }

      else if ( ntot == resid.size() )
      {  // Compute the projected gradient and gain of each simulated point
         QVector< double > pcol( ntot );

         for ( int cc = 0; cc < ncols; cc++ )
         {
            project_noise( amat.data() + cc * ntot, pcol.data(),
                           nscans, npoints, noisflag );
            double grad    = 0.0;
            double pnorm   = 0.0;

            for ( int ii = 0; ii < ntot; ii++ )
            {
               grad          += pcol[ ii ] * resid[ ii ];
               pnorm         += pcol[ ii ] * pcol[ ii ];
            }

            // Remove the span of the solution's columns for the estimate
            int nq         = qbasis.size() / ntot;

            for ( int qq = 0; qq < nq; qq++ )
            {
               const double* qv = qbasis.constData() + qq * ntot;
               double dot     = 0.0;
               for ( int ii = 0; ii < ntot; ii++ )
                  dot           += qv[ ii ] * pcol[ ii ];
               for ( int ii = 0; ii < ntot; ii++ )
                  pcol[ ii ]    -= dot * qv[ ii ];
            }

            double qnorm   = 0.0;
            double qgrad   = 0.0;
            for ( int ii = 0; ii < ntot; ii++ )
            {
               qnorm         += pcol[ ii ] * pcol[ ii ];
               qgrad         += pcol[ ii ] * resid[ ii ];
            }

            US_OptimalityPoint opt;
            opt.sol        = usols[ cc ];
            opt.sol.c      = 0.0;
            opt.gradient   = grad;
            opt.gain_min   = ( grad > 0.0  &&  pnorm > 0.0 )
                             ? ( grad * grad / pnorm ) : 0.0;
            // Estimate:  reduction by the part of the column outside the
            // solution and noise span (0 if (nearly) inside it)
            opt.gain       = ( grad > 0.0  &&  qgrad > 0.0  &&
                               qnorm > pnorm * 1.0e-6 )
                             ? ( qgrad * qgrad / qnorm ) : 0.0;
            opt.insol      = false;
            opt.cut        = false;
            points << opt;
         }

         // Points without a simulated column have no usable signal
         for ( int jj = 0; jj < bsols.size(); jj++ )
         {
            if ( usols.contains( bsols[ jj ] ) )
               continue;

            US_OptimalityPoint opt;
            opt.sol        = bsols[ jj ];
            opt.sol.c      = 0.0;
            opt.gradient   = 0.0;
            opt.gain       = 0.0;
            opt.gain_min   = 0.0;
            opt.insol      = false;
            opt.cut        = true;
            points << opt;
         }
      }

      emit batch_done( 1 );
   }
}

// Check constructor
US_OptimalityCheck2D::US_OptimalityCheck2D( SS_DATASET* dset,
      const QList< QVector< US_Solute > >& grid,
      const QVector< US_Solute >& finals,
      const QVector< double >& ti_noise, const QVector< double >& ri_noise,
      int nthreads, QObject* parent )
   : QObject( parent ), dset( dset ), grid( grid ), finals( finals ),
     ti_noise( ti_noise ), ri_noise( ri_noise ), nthreads( nthreads )
{
   dset_cp        = *dset;
   noisflag       = ( ti_noise.isEmpty() ? 0 : 1 )
                  + ( ri_noise.isEmpty() ? 0 : 2 );
   nthreads       = qMax( 1, nthreads );
   ssq_fit        = 0.0;
   ssq_noise      = 0.0;
   ssq_base       = 0.0;
   ndata          = 0;
   nbatches       = 0;
   kbatches       = 0;
   kworkers       = 0;
}

US_OptimalityCheck2D::~US_OptimalityCheck2D()
{
   stop();
}

// Start by simulating the final solutes to rebuild the residual
void US_OptimalityCheck2D::start()
{
   error.clear();
   points.clear();

   if ( finals.isEmpty()  ||  grid.isEmpty() )
   {
      error          = tr( "No final fit solutes or grid points to check." );
      QTimer::singleShot( 0, this, &US_OptimalityCheck2D::finished );
      return;
   }

   nbatches       = grid.size() + 2;
   refits.clear();
   kbatches       = 0;
   emit progress( kbatches, nbatches );

   QList< QVector< US_Solute > > fbatch;
   fbatch << finals;
   US_OptimalityWorker* wrk = new US_OptimalityWorker( &dset_cp, fbatch,
                                 QVector< double >(), noisflag, 1 );
   workers << wrk;
   connect( wrk, &QThread::finished,
            this, &US_OptimalityCheck2D::final_done );
   wrk->start();
}

// Stop all workers
void US_OptimalityCheck2D::stop()
{
   for ( int ii = 0; ii < workers.size(); ii++ )
   {
      workers[ ii ]->disconnect();
      workers[ ii ]->flag_abort();
   }

   for ( int ii = 0; ii < workers.size(); ii++ )
   {
      workers[ ii ]->wait();
      delete workers[ ii ];
   }

   workers.clear();
}

// Final solutes simulated:  rebuild the residual and start the grid workers
void US_OptimalityCheck2D::final_done()
{
   US_OptimalityWorker* wrk = workers.takeFirst();
   QVector< double > bvec   = wrk->bvec;
   QVector< double > acols  = wrk->acols;
   QVector< US_Solute > cs  = wrk->csols;
   wrk->wait();
   delete wrk;

   int nscans     = dset_cp.run_data.scanCount();
   int npoints    = dset_cp.run_data.pointCount();
   ndata          = bvec.size();

   if ( ndata == 0  ||  ndata != nscans * npoints )
   {  // Only a single data set with a simple layout is supported
      error          = tr( "Unable to simulate the final fit solutes." );
      emit finished();
      return;
   }

   // Residual:  data - sum of concentration * column - noise
   resid          = bvec;

   for ( int cc = 0; cc < cs.size(); cc++ )
   {
      double conc    = 0.0;

      for ( int jj = 0; jj < finals.size(); jj++ )
      {
         if ( same_point( cs[ cc ], finals[ jj ] ) )
         {
            conc           = finals[ jj ].c;
            break;
         }
      }

      const double* col = acols.constData() + cc * ndata;

      for ( int ii = 0; ii < ndata; ii++ )
         resid[ ii ]   -= conc * col[ ii ];
   }

   for ( int ss = 0; ss < nscans; ss++ )
   {
      double rinoi   = ( ss < ri_noise.size() ) ? ri_noise[ ss ] : 0.0;

      for ( int rr = 0; rr < npoints; rr++ )
      {
         double tinoi   = ( rr < ti_noise.size() ) ? ti_noise[ rr ] : 0.0;
         resid[ ss * npoints + rr ] -= ( tinoi + rinoi );
      }
   }

   ssq_fit        = 0.0;
   for ( int ii = 0; ii < ndata; ii++ )
      ssq_fit       += resid[ ii ] * resid[ ii ];

   // How much refitting the noise alone would remove
   ssq_noise      = 0.0;
   if ( noisflag != 0 )
   {
      QVector< double > presid( ndata );
      project_noise( resid.constData(), presid.data(), nscans, npoints,
                     noisflag );
      double ssq_p   = 0.0;
      for ( int ii = 0; ii < ndata; ii++ )
         ssq_p         += presid[ ii ] * presid[ ii ];
      ssq_noise      = qMax( 0.0, ssq_fit - ssq_p );
   }

   // Orthonormal basis of the solution's noise-projected columns
   // (modified Gram-Schmidt, applied twice for stability)
   qbasis.clear();
   QVector< double > pcol( ndata );

   for ( int cc = 0; cc < cs.size(); cc++ )
   {
      project_noise( acols.constData() + cc * ndata, pcol.data(),
                     nscans, npoints, noisflag );
      double norm0   = 0.0;
      for ( int ii = 0; ii < ndata; ii++ )
         norm0         += pcol[ ii ] * pcol[ ii ];
      int nq         = qbasis.size() / ndata;

      for ( int pass = 0; pass < 2; pass++ )
      {
         for ( int qq = 0; qq < nq; qq++ )
         {
            const double* qv = qbasis.constData() + qq * ndata;
            double dot     = 0.0;
            for ( int ii = 0; ii < ndata; ii++ )
               dot           += qv[ ii ] * pcol[ ii ];
            for ( int ii = 0; ii < ndata; ii++ )
               pcol[ ii ]    -= dot * qv[ ii ];
         }
      }

      double norm1   = 0.0;
      for ( int ii = 0; ii < ndata; ii++ )
         norm1         += pcol[ ii ] * pcol[ ii ];

      if ( norm1 > norm0 * 1.0e-12 )
      {  // Independent enough to add to the basis
         double scale   = 1.0 / sqrt( norm1 );
         for ( int ii = 0; ii < ndata; ii++ )
            qbasis        << pcol[ ii ] * scale;
      }
   }

   kbatches++;
   emit progress( kbatches, nbatches );

   // Distribute the subgrids (batches) over the worker threads
   int nwork      = qMin( nthreads, grid.size() );
   kworkers       = 0;

   for ( int ww = 0; ww < nwork; ww++ )
   {
      QList< QVector< US_Solute > > wbatches;

      for ( int bb = ww; bb < grid.size(); bb += nwork )
         wbatches << grid[ bb ];

      US_OptimalityWorker* gwrk = new US_OptimalityWorker( &dset_cp,
                                     wbatches, resid, noisflag, ww + 1,
                                     qbasis );
      workers << gwrk;
      connect( gwrk, &US_OptimalityWorker::batch_done,
               this, &US_OptimalityCheck2D::batch_done );
      connect( gwrk, &QThread::finished,
               this, &US_OptimalityCheck2D::worker_done );
   }

   for ( int ww = 0; ww < workers.size(); ww++ )
      workers[ ww ]->start();
}

// A batch of grid points is done
void US_OptimalityCheck2D::batch_done( int nbat )
{
   kbatches      += nbat;
   emit progress( kbatches, nbatches );
}

// A grid worker is done:  when all are, collect the results
void US_OptimalityCheck2D::worker_done()
{
   if ( ++kworkers < workers.size() )
      return;

   for ( int ww = 0; ww < workers.size(); ww++ )
   {
      workers[ ww ]->wait();
      points       += workers[ ww ]->points;
      delete workers[ ww ];
   }
   workers.clear();

   // Points in the final solution are optimal by construction
   for ( int ii = 0; ii < points.size(); ii++ )
   {
      for ( int jj = 0; jj < finals.size(); jj++ )
      {
         if ( same_point( points[ ii ].sol, finals[ jj ] ) )
         {
            points[ ii ].insol = true;
            points[ ii ].gain  = 0.0;
            points[ ii ].gain_min = 0.0;
            break;
         }
      }
   }

   // Verify the estimates:  refit the final solutes alone and with each of
   // the most promising points, as the 2DSA would
   const int ntop = 5;
   QList< int > order;
   for ( int ii = 0; ii < points.size(); ii++ )
      if ( points[ ii ].gain > ssq_fit * 1.0e-9 )  order << ii;
   std::sort( order.begin(), order.end(), [ this ]( int a, int b )
              { return points[ a ].gain > points[ b ].gain; } );
   order         = order.mid( 0, ntop );

   QList< QVector< US_Solute > > rbatches;
   QList< int > rids;
   QVector< US_Solute > fsols = finals;
   for ( int jj = 0; jj < fsols.size(); jj++ )
      fsols[ jj ].c  = 0.0;
   rbatches << fsols;
   rids     << -1;

   for ( int ii = 0; ii < order.size(); ii++ )
   {
      rbatches << ( fsols + QVector< US_Solute >{ points[ order[ ii ] ].sol } );
      rids     << order[ ii ];
   }

   int nwork      = qMin( nthreads, rbatches.size() );
   kworkers       = 0;

   for ( int ww = 0; ww < nwork; ww++ )
   {
      QList< QVector< US_Solute > > wbatches;
      QList< int > wids;

      for ( int bb = ww; bb < rbatches.size(); bb += nwork )
      {
         wbatches << rbatches[ bb ];
         wids     << rids[ bb ];
      }

      US_OptimalityWorker* rwrk = new US_OptimalityWorker( &dset_cp,
                                     wbatches, QVector< double >(),
                                     noisflag, ww + 1 );
      rwrk->set_refit( wids );
      workers << rwrk;
      connect( rwrk, &QThread::finished,
               this, &US_OptimalityCheck2D::refit_done );
   }

   for ( int ww = 0; ww < workers.size(); ww++ )
      workers[ ww ]->start();
}

// Refit workers are done:  collect the results
void US_OptimalityCheck2D::refit_done()
{
   if ( ++kworkers < workers.size() )
      return;

   refits.clear();
   ssq_base       = ssq_fit;

   for ( int ww = 0; ww < workers.size(); ww++ )
   {
      workers[ ww ]->wait();

      for ( int ii = 0; ii < workers[ ww ]->refits.size(); ii++ )
      {
         QPair< int, double > rf = workers[ ww ]->refits[ ii ];
         if ( rf.first < 0 )
            ssq_base       = rf.second;
         else
            refits << rf;
      }

      delete workers[ ww ];
   }
   workers.clear();

   std::sort( refits.begin(), refits.end(),
              []( const QPair< int, double >& a, const QPair< int, double >& b )
              { return a.second < b.second; } );
   kbatches       = nbatches;
   emit progress( kbatches, nbatches );
   emit finished();
}

// Compose a text summary of the results
QString US_OptimalityCheck2D::summary()
{
   if ( ! error.isEmpty() )
      return error;

   int    ninsol   = 0;
   int    ncut     = 0;
   int    nimprv   = 0;
   int    nimprv3  = 0;
   double dnorm    = 1.0 / (double)qMax( 1, ndata );
   double rmsd     = sqrt( ssq_fit * dnorm );

   for ( int ii = 0; ii < points.size(); ii++ )
   {
      const US_OptimalityPoint& opt = points[ ii ];
      if ( opt.insol )  ninsol++;
      if ( opt.cut   )  ncut++;
      if ( opt.gain > ssq_fit * 1.0e-9 )  nimprv++;
      if ( opt.gain > ssq_fit * 1.0e-3 )  nimprv3++;
   }

   QString text = tr( "Final fit RMSD %1; %2 grid points (%3 in the solution, "
                      "%4 without signal).\n" )
      .arg( rmsd, 0, 'e', 5 ).arg( points.size() ).arg( ninsol ).arg( ncut );

   if ( ssq_noise > ssq_fit * 1.0e-6 )
   {
      text += tr( "The fitted noise is not optimal for this solution: "
                  "refitting the noise alone would give RMSD %1.\n" )
         .arg( sqrt( ( ssq_fit - ssq_noise ) * dnorm ), 0, 'e', 5 );
   }

   if ( nimprv == 0 )
   {
      text += tr( "No grid point can lower the RMSD:  the fit is optimal "
                  "for this grid." );
      return text;
   }

   // Verdict from the refits (the estimates ignore positivity limits and
   // the approximations of the noise fit)
   double rbase    = sqrt( ssq_base * dnorm );
   double rbest    = refits.isEmpty() ? rbase
                     : sqrt( refits[ 0 ].second * dnorm );
   double rchange  = ( rbest - rbase ) / rbase * 100.0;

   // Changes below 0.001% of the RMSD are within the accuracy of the
   // NNLS and (approximate) noise fit, which can even make them positive
   if ( rbest < rbase * ( 1.0 - 1.0e-5 ) )
   {
      const US_Solute& sol = points[ refits[ 0 ].first ].sol;
      text += tr( "NOT OPTIMAL:  adding s=%1 f/f0=%2 to the solution lowers "
                  "the RMSD by %3%.\n" )
         .arg( sol.s * 1.0e+13, 0, 'g', 5 ).arg( sol.k, 0, 'g', 5 )
         .arg( -rchange, 0, 'f', 4 );
   }
   else
   {
      text += tr( "OPTIMAL within the accuracy of the NNLS and noise fit:  "
                  "the best refit with the most promising points changes "
                  "the RMSD by %1%.\n" ).arg( rchange, 0, 'f', 4 );
   }

   text += tr( "%1 grid points have an improving direction (%2 with an "
               "estimated gain of 0.1% or more of the squared residual).\n" )
      .arg( nimprv ).arg( nimprv3 );
   text += tr( "Refits of the top %1 (refit of the solution alone: RMSD %2):" )
      .arg( refits.size() ).arg( rbase, 0, 'e', 5 );

   for ( int ii = 0; ii < refits.size(); ii++ )
   {
      const US_Solute& sol = points[ refits[ ii ].first ].sol;
      double rrmsd   = sqrt( refits[ ii ].second * dnorm );
      text += tr( "\n  + s=%1 f/f0=%2 vbar=%3:  RMSD %4 (%5%)" )
         .arg( sol.s * 1.0e+13, 0, 'g', 5 ).arg( sol.k, 0, 'g', 5 )
         .arg( sol.v, 0, 'g', 4 ).arg( rrmsd, 0, 'e', 5 )
         .arg( ( rrmsd - rbase ) / rbase * 100.0, 0, 'f', 4 );
   }

   return text;
}
