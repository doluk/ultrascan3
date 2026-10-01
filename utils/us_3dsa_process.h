//! \file us_3dsa_process.h
#ifndef US_3DSA_PROCESS_H
#define US_3DSA_PROCESS_H

#include <QtCore>

#include "us_extern.h"
#include "us_model.h"
#include "us_solute.h"
#include "us_solve_sim.h"

//! \brief 3DSA fit engine
//!
//! Fits a three-dimensional grid -- by default sedimentation coefficient,
//! frictional ratio and partial specific volume -- across a series of data
//! sets, by non-negative least squares.
//!
//! Unlike US_2dsaProcess this class carries no GUI dependency, so the same
//! engine serves the desktop program, the MPI back end and the test suite.
//!
//! \par Why a series
//! The Lamm-equation solver reads a species only through its
//! experimental-space s and D, and vbar reaches s alone, through the buoyancy
//! term.  On one data set the grid is therefore exactly rank-deficient in
//! vbar: a one-parameter family of (s, f/f0, vbar) triples produces identical
//! simulated data.  Only a series of runs in buffers of differing density
//! separates them.  fit() refuses a series whose buoyancy contrast is below
//! US_SolveSim::VBAR_CONTRAST_REFUSE unless the caller explicitly overrides
//! it.  See doc/develop/3dsa_design.md.
class US_UTIL_EXTERN US_3dsaProcess : public QObject
{
   Q_OBJECT

   public:

      //! \brief Inputs for one fit
      class US_UTIL_EXTERN Parameters
      {
         public:
            Parameters();

            double x_min;      //!< X axis minimum (s, in s units e.g. 1e-13)
            double x_max;      //!< X axis maximum
            int    x_res;      //!< X axis grid points

            double y_min;      //!< Y axis minimum (f/f0, or D)
            double y_max;      //!< Y axis maximum
            int    y_res;      //!< Y axis grid points

            double z_min;      //!< Z axis minimum (vbar20)
            double z_max;      //!< Z axis maximum
            int    z_res;      //!< Z axis grid points

            int    grid_reps;  //!< Subgrid repetitions per axis
            int    s_mask;     //!< Attribute mask, ( x << 6 ) | ( y << 3 ) | z
            int    nthreads;   //!< Worker threads (1 runs serially)
            int    noisflag;   //!< Noise flag: 0-3 for none|ti|ri|both
            int    max_tsols;  //!< Maximum solutes per task beyond depth 0
            double alpha;      //!< Tikhonov regularization factor

            //! Fit one amplitude factor per data set, alternating with the
            //! NNLS.  A contrast series loads each cell separately, so
            //! without this the loading differences bias the fitted vbar.
            bool   fit_scales;
            int    scale_iters;   //!< Maximum scale-factor iterations
            double scale_toler;   //!< Relative convergence tolerance

            //! Proceed even when the buoyancy contrast is below the refusal
            //! threshold.  The fitted vbar is then not a measurement; this
            //! exists for tests and for deliberate diagnostic runs.
            bool   ignore_contrast;
      };

      //! \brief Outputs of one fit
      class US_UTIL_EXTERN Result
      {
         public:
            Result();

            US_Model          model;      //!< Fitted model, standard (20W) space
            QVector< double > scales;     //!< Fitted per-data-set amplitudes
            QVector< double > ti_noise;   //!< Time-invariant noise, or empty
            QVector< double > ri_noise;   //!< Radially-invariant noise
            QVector< double > variances;  //!< Variance per data set
            double            variance;   //!< Total variance
            double            rmsd;       //!< Total RMSD
            double            contrast;   //!< Buoyancy contrast of the series
            double            vbar_resol; //!< Implied vbar resolution, mL/g
            int               ngrid;      //!< Grid points generated
            int               nsubgrids;  //!< Depth-0 subgrids
            int               ndepths;    //!< Depth levels executed
            int               ntasks;     //!< NNLS solves performed
            int               nsimul;     //!< Lamm-equation solves performed
            int               nscaliter;  //!< Scale-factor iterations run
            qint64            msecs;      //!< Wall-clock time
            QString           report;     //!< Human-readable summary
      };

      //! \brief Create a 3DSA fit engine
      //! \param dsets   Data sets in the series; not modified by the fit
      //! \param parent  Parent object
      US_3dsaProcess( QList< US_SolveSim::DataSet* >&, QObject* = 0 );

      //! \brief Run a fit to completion
      //! \param parms   Fit inputs
      //! \param result  Fit outputs, valid only when this returns true
      //! \returns       True on success; lastError() explains a false
      bool fit( const Parameters&, Result& );

      //! \brief Ask a running fit to stop at the next opportunity
      void abort_fit( void );

      //! \brief Message describing the last failure
      QString lastError( void ) const { return errMsg; }

      //! \brief The default axis mask: s, f/f0, vbar
      static int mask_s_k_v( void );

      //! \brief The alternate axis mask: s, D, vbar
      static int mask_s_d_v( void );

   signals:
      //! \brief Progress: tasks completed, tasks expected
      void progress_update( int, int );

      //! \brief A human-readable status line
      void message_update( QString );

   private:

      //! One unit of NNLS work
      struct Task
      {
         QVector< US_Solute > isolutes;   // Input solutes
         QVector< double >    scales;     // Per-data-set amplitude factors
         int                  noisflag = 0;      // Noise flag for this task
         bool                 keep_sim = false;  // Retain simulation, noise
      };

      //! What a task produced
      struct TaskResult
      {
         QVector< US_Solute > csolutes;   // Surviving solutes
         QVector< double >    ti_noise;
         QVector< double >    ri_noise;
         QVector< double >    variances;
         double               variance = 0.0;
         int                  nsimul   = 0;
         bool                 ok       = false;
         QString              error;
         US_DataIO::RawData   sim_data;   // Only when keep_sim
      };

      // Run one level of tasks, in parallel when nthreads > 1
      bool run_level( const QVector< Task >&, QVector< TaskResult >&,
                      const Parameters& );

      // Run a single task on one thread's private data-set copies
      void run_task( int, const Task&, TaskResult&, const Parameters& );

      // Private per-thread copies of the data sets, made once per fit
      void make_thread_datasets( int );

      // Per-data-set amplitudes from a completed simulation
      bool update_scales( US_DataIO::RawData&, QVector< double >&,
                          double& );

      // Accelerate the amplitude sequence, which otherwise converges only
      // geometrically.  Returns true when an extrapolation was applied.
      static bool extrapolate_scales( QVector< double >&,
                                      const QVector< double >&,
                                      const QVector< double >& );

      // Build the output model from the final solutes
      bool build_model( const QVector< US_Solute >&, const Parameters&,
                        US_Model& );

      // Split solutes into tasks of at most max_tsols
      QVector< Task > repartition( const QVector< US_Solute >&, int,
                                   const QVector< double >& );

      QList< US_SolveSim::DataSet* >&               dsets;
      QVector< QList< US_SolveSim::DataSet* > >     thr_dsets;
      QVector< QVector< US_SolveSim::DataSet > >    thr_store;

      QString       errMsg;
      QAtomicInt    abort_flag;
      int           dbg_level;
      int           tasks_done;
      int           tasks_expect;
};
#endif
