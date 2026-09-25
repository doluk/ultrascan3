//! \file us_optimality_2d.h
#ifndef US_OPTIMALITY_2D_H
#define US_OPTIMALITY_2D_H

#include <QtCore>

#include "us_extern.h"
#include "us_solve_sim.h"
#include "us_solute.h"

#ifndef SS_DATASET
#define SS_DATASET US_SolveSim::DataSet
#endif

//! \brief Optimality test result for one grid point
struct US_OptimalityPoint
{
   US_Solute sol;       //!< Grid point
   double    gradient;  //!< Projected gradient (column . residual)
   double    gain;      //!< Estimated residual sum-of-squares reduction
                        //!<  (with the solution's concentrations refitted)
   double    gain_min;  //!< Guaranteed reduction (all else held fixed)
   bool      insol;     //!< Flag: point is in the final solution
   bool      cut;       //!< Flag: point has no usable signal (norm cut)
};

//! \brief Worker thread simulating grid point batches for the check
class US_OptimalityWorker : public QThread
{
   Q_OBJECT

   public:
      //! \brief Create a worker
      //! \param dset     Data set (copied)
      //! \param batches  Solute batches this worker simulates
      //! \param resid    Residual vector (empty to return raw columns)
      //! \param noisflag Noise flag of the fit: 0-3 for none|ti|ri|both
      //! \param thrn     Thread number (1,...)
      //! \param qbasis   Orthonormal basis of the solution's noise-projected
      //!                 columns (column-major, may be empty)
      US_OptimalityWorker( SS_DATASET*, const QList< QVector< US_Solute > >&,
                           const QVector< double >&, int, int,
                           const QVector< double >& = QVector< double >() );

      //! \brief Make this a refit worker:  fit each batch by NNLS (with the
      //!        fit's noise) and record its residual sum of squares
      void set_refit( const QList< int >& );

      //! \brief Use a simulation cache (or none with 0)
      void set_cache( US_SolveSim::SimCache* cache ) { simcache = cache; }

      QList< QPair< int, double > > refits; //!< Refit (batch id, ssq)

      //! \brief Flag the worker to stop after the current batch
      void flag_abort( void ) { abort = true; }

      QList< US_OptimalityPoint > points;   //!< Results (with residual)
      QVector< US_Solute >        csols;    //!< Simulated solutes (no resid)
      QVector< double >           acols;    //!< Their columns (no residual)
      QVector< double >           bvec;     //!< Data vector B of the fit

   signals:
      //! \brief Signal that a batch has been processed
      void batch_done( int );

   protected:
      void run( void );

   private:
      bool simulate( const QVector< US_Solute >&, QVector< double >&,
                     QVector< double >& );

   private:
      SS_DATASET                     dset_wk;
      QList< QVector< US_Solute > >  batches;
      QVector< double >              resid;
      QVector< double >              qbasis;
      int                            noisflag;
      int                            thrn;
      bool                           abort;
      bool                           refit;
      QList< int >                   bids;
      US_SolveSim::SimCache*         simcache = nullptr;
};

//! \brief Test how close a final 2DSA fit is to the optimum of the full
//!        grid NNLS problem.
//!
//! The full grid problem minimizes |b - A x - noise|^2 with x >= 0. At its
//! optimum every grid point j not in the solution has (P A_j) . r <= 0,
//! where r is the residual and P removes the part of a column that the
//! fitted noise can absorb. A point with g_j = (P A_j) . r > 0 could still
//! lower the residual:  adding it alone (with the noise refitted) lowers the
//! residual sum of squares by at least g_j^2 / |P A_j|^2. Since grid columns
//! are strongly correlated, a better estimate also refits the solution:
//! g_j^2 / |Q A_j|^2, where Q also removes the span of the solution's columns
//! (exact while the solution's concentrations stay positive).
class US_OptimalityCheck2D : public QObject
{
   Q_OBJECT

   public:
      //! \brief Create a check for a fit
      //! \param dset      Data set of the final fit
      //! \param grid      Grid points as a list of subgrids
      //! \param finals    Final solutes (with concentrations)
      //! \param ti_noise  Time-invariant noise of the final fit (or empty)
      //! \param ri_noise  Radially-invariant noise of the final fit (or empty)
      //! \param nthreads  Number of threads to use
      //! \param parent    Parent object
      US_OptimalityCheck2D( SS_DATASET*,
                            const QList< QVector< US_Solute > >&,
                            const QVector< US_Solute >&,
                            const QVector< double >&,
                            const QVector< double >&, int,
                            QObject* = 0, US_SolveSim::SimCache* = 0 );
      ~US_OptimalityCheck2D();

      //! \brief Start the check (asynchronous)
      void start( void );

      //! \brief Stop the check
      void stop( void );

      QList< US_OptimalityPoint > points;   //!< Results for all grid points
      double  ssq_fit;       //!< Residual sum of squares (recomputed)
      double  ssq_noise;     //!< Reduction from refitting the noise alone
      double  ssq_base;      //!< Refit of the final solutes alone
      QList< QPair< int, double > > refits; //!< Refits (point index, ssq)
      int     ndata;         //!< Number of data values
      QString error;         //!< Error message (empty if OK)

      //! \brief Compose a text summary of the results
      QString summary( void );

   signals:
      //! \brief Progress:  batches done, batches total
      void progress( int, int );
      //! \brief The check is complete (or failed; see error)
      void finished( void );

   private:
      SS_DATASET*                    dset;
      SS_DATASET                     dset_cp;
      QList< QVector< US_Solute > >  grid;
      QVector< US_Solute >           finals;
      QVector< double >              ti_noise;
      QVector< double >              ri_noise;
      QVector< double >              resid;
      QVector< double >              qbasis;
      QList< US_OptimalityWorker* >  workers;
      US_SolveSim::SimCache*         simcache;
      int                            nthreads;
      int                            noisflag;
      int                            nbatches;
      int                            kbatches;
      int                            kworkers;

   private slots:
      void final_done ( void );
      void batch_done ( int  );
      void worker_done( void );
      void refit_done ( void );
};
#endif
