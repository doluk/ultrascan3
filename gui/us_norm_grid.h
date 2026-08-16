//! \file us_norm_grid.h
#ifndef US_NORM_GRID_H
#define US_NORM_GRID_H

#include <QtCore>

#include "us_extern.h"
#include "us_solve_sim.h"
#include "us_solute.h"
#include "us_model.h"
#include "us_buffer.h"
#include "us_dataIO.h"
#include "us_show_norm.h"
#include "us_worker_calcnorm.h"

//! \brief Computes the A-matrix column norms of a solute grid and shows them
//!
//! Every point of an s by f/f0 (or s by vbar) grid is simulated as a single
//! species at unit loading concentration.  The L2 norm of that simulation is
//! the norm of the corresponding column of the A matrix an NNLS fit is
//! handed, so the resulting map says which parts of the grid can contribute
//! signal at all, and the accompanying coherences say which parts a fit
//! cannot tell apart.
//!
//! The columns are built the way a fit would build them:  the data set's own
//! buffer corrections, simulation parameters, mesh type and solver, band
//! forming volume and thresholds, and OD limit all apply.  Anything holding
//! a US_SolveSim::DataSet can therefore ask for a norm grid describing the
//! fit it would run.
class US_GUI_EXTERN US_NormGrid : public QObject
{
   Q_OBJECT

   public:
      //! \param parent  Parent object
      US_NormGrid( QObject* parent = 0 );
      ~US_NormGrid();

      //! \brief Definition of the grid to compute norms over
      class US_GUI_EXTERN GridDef
      {
         public:
            GridDef() : slo( 1.0e-13 ), sup( 1.0e-12 ), nss( 40 ),
                        klo( 1.0 ), kup( 4.0 ), nks( 40 ), cff0( 0.0 ),
                        nthrd( 1 ) {}

            double slo;    //!< s lower limit, in seconds (not 1e-13 units)
            double sup;    //!< s upper limit, in seconds
            int    nss;    //!< number of steps in the s direction
            double klo;    //!< f/f0 (or vbar) lower limit
            double kup;    //!< f/f0 (or vbar) upper limit
            int    nks;    //!< number of steps in the second direction
            //! Constant f/f0.  Zero means the grid varies f/f0 at constant
            //!  vbar; a positive value means it varies vbar at this f/f0.
            double cff0;
            int    nthrd;  //!< worker thread count
      };

      //! \brief Build a data set from a simulation, for programs that have
      //!        no experiment data of their own
      //!
      //! The norm columns are simulated onto the radial and scan grid of
      //! the supplied data, with the buffer corrections and simulation
      //! parameters that produced it, so the columns match what that
      //! simulation would contribute to a fit.
      //! \param dset      Data set to fill in
      //! \param model     Model whose vbar the corrections are taken from
      //! \param buffer    Solution buffer
      //! \param simparams Simulation parameters
      //! \param simdata   Simulated data defining the radial and scan grid
      //! \param errmsg    Returned reason when the data cannot be used
      //! \returns         Flag that the data set was built
      static bool dataset_from_sim( US_SolveSim::DataSet&, const US_Model&,
                                    const US_Buffer&,
                                    const US_SimulationParameters&,
                                    US_DataIO::RawData&, QString& );

      //! \brief Grid limits spanning a model's own components, padded out
      //! \param model  Model to take s and f/f0 ranges from
      //! \param grid   Grid definition whose limits are set
      static void grid_from_model( US_Model&, GridDef& );

      //! \brief Start computing norms for a model's own species
      //!
      //! Instead of covering a grid, this simulates exactly the species the
      //! model contains.  It answers how well an experimental design can
      //! capture each species and tell them apart from one another, rather
      //! than what a fit over a whole parameter range could resolve.
      //! \param dset    Data set the columns are built from
      //! \param model   Model whose components are simulated
      //! \param nthrd   Worker thread count
      //! \param parentw Parent widget of the viewer window
      //! \param errmsg  Returned reason when the start is refused
      //! \returns       Flag that the calculation was started
      bool start_species( US_SolveSim::DataSet*, US_Model&, int, QWidget*,
                          QString& );

      //! \brief Start computing a norm grid
      //!
      //! Returns immediately; the calculation runs in worker threads and
      //! the viewer is shown when they finish.  Only one calculation may be
      //! in flight at a time.
      //! \param dset    Data set the columns are built from
      //! \param grid    Grid definition
      //! \param parentw Parent widget of the viewer window
      //! \param errmsg  Returned reason when the start is refused
      //! \returns       Flag that the calculation was started
      bool start( US_SolveSim::DataSet*, const GridDef&, QWidget*,
                  QString& );

      //! \brief Supply the band-forming gradient and co-sedimenting data
      //!        the finite volume solver should apply
      //!
      //! A fit over a buffer with co-diffusing or co-sedimenting components
      //! builds these and hands them to the solver.  Without them the norm
      //! columns are simulated in a plain buffer, which the viewer says so
      //! that the difference is not read as a property of the grid.
      //! \param bfg  Band-forming gradient, or null
      //! \param csd  Co-sedimenting component simulation, or null
      void set_cosed( US_Math_BF::Band_Forming_Gradient*,
                      US_LammAstfvm::CosedData* );

      //! \brief Flag that a calculation is currently running
      bool is_busy( void ) const;

      //! \brief Total grid points of the running calculation, for a
      //!        progress bar maximum
      int  total_steps( void ) const;

      //! \brief The viewer window, once shown, or null
      US_show_norm* viewer( void ) const;

      //! \brief Close any open viewer window
      void close_viewer( void );

   signals:
      //! \brief Emitted as grid points complete, with the step increment
      void norm_progress ( int );
      //! \brief Emitted once the calculation is done and the viewer is shown
      void norm_finished ( void );

   private slots:
      void worker_progress( int );
      void worker_complete( WorkerThreadCalcNorm* );

   private:
      void resolve_timestate( US_SolveSim::DataSet* );
      //! \brief Common tail of start() and start_species()
      bool launch( QVector< US_Solute >&, int, int, bool, double, int,
                   QWidget*, QString& );

      US_SolveSim::DataSet*    dset;
      US_Model                 model2;
      GridDef                  gdef;
      bool                     specmode;
      QPointer< QWidget >      parentw;
      QPointer< US_show_norm > analcd;

      int     kthrdr;      // worker threads still running
      int     nsolutes;    // total grid points
      int     nrm_nss;     // grid points per row (X direction)
      int     nrm_nks;     // grid rows (Y direction)
      int     nrm_nsamp;   // values per column signature
      int     nrm_nrad;    // radial points per signature
      int     nrm_nscn;    // scans per signature
      int     nrm_rstr;    // radial stride of a signature
      int     nrm_sstr;    // scan stride of a signature
      int     dbg_level;
      bool    busy;

      US_Math_BF::Band_Forming_Gradient* bfgrad;
      US_LammAstfvm::CosedData*          cosedd;

      QVector< double >  nrm_coher_x;
      QVector< double >  nrm_coher_y;
      QVector< double >  nrm_signorm;
      QVector< float >   nrm_sigs;
};
#endif

