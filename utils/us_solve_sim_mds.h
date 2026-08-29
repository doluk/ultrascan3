//! \file us_solve_sim_mds.h
#ifndef US_SOLVE_SIM_MDS_H
#define US_SOLVE_SIM_MDS_H

#include <QtCore>
#include <algorithm>

#include "us_model.h"
#include "us_dataIO.h"
#include "us_noise.h"
#include "us_simparms.h"
#include "us_solution.h"
#include "us_solute.h"
#include "us_zsolute.h"
#include "us_astfem_math.h"
#include "us_astfem_rsa.h"
#include "us_constants.h"
#include "us_solve_sim.h"

//! \brief Solve a simulation over a series of data sets, fitting time- and
//! radially-invariant noise separately for each one
//!
//! This is a copy of US_SolveSim whose noise algebra is data-set aware.
//!
//! US_SolveSim sizes its TI and RI noise vectors for the whole series --
//! \c ntinois sums \c pointCount() over every data set and \c nrinois sums
//! \c scanCount() -- and its residual loop indexes them per data set.  But
//! every routine that *computes* the noise reads \c data_sets[ d_offs ], the
//! first data set, and loops over that set's point and scan counts alone.  So
//! only the first block of each vector is ever written; the rest stay zero and
//! no noise is subtracted from those data sets.  On a single data set the two
//! classes agree exactly, which is why the defect went unnoticed: 2DSA, PCSA,
//! GA and DMGA all fit one experiment at a time in the common case.
//!
//! Systematic noise is a property of the optics and the cell, not of the
//! analyte, so two runs -- different rotor holes, different windows, different
//! days -- have unrelated TI and RI vectors.  A global fit that forces one
//! vector on all of them is fitting the wrong model.  3DSA is global by
//! construction (vbar is only measurable across a buoyancy-contrast series),
//! so it cannot use the single-data-set path at all.
//!
//! What changes here, and only this:
//!
//! \li The noise vectors are the concatenation of one block per data set --
//!     \c npoints values of TI and \c nscans values of RI each -- which is
//!     already the layout US_SolveSim's residual loop assumes.
//! \li Every average that enters the noise algebra is taken within a data set:
//!     "a~" over that set's radial points, "a-bar" over that set's scans.
//! \li The reduced normal equations that eliminate the noise sum over the
//!     whole series, but subtract each element's own data set's means.  For a
//!     balanced scan x point grid -- which each data set is -- the two-way
//!     within transform is exact, so this is the exact profile likelihood for
//!     the concentrations with per-data-set noise projected out, not an
//!     approximation.
//!
//! Everything else -- the A-matrix build, the Lamm solutions, the amplitude
//! scale factors, regularization, band-forming thresholds, the residual and
//! variance loops -- is the same code.  US_SolveSim is left untouched so that
//! 2DSA, PCSA, GA and DMGA are unaffected.
//!
//! The data classes are shared with US_SolveSim rather than duplicated, so a
//! caller can hand the same DataSet list and Simulation object to either.
//!
//! \note The analysis helpers checkGridSize(), buoyancy_contrast() and
//! vbar_resolution() are not duplicated here; call them on US_SolveSim.

class US_UTIL_EXTERN US_SolveSimMDS : public QObject
{
 Q_OBJECT

  public:

    //! Input data set, shared with US_SolveSim
    typedef US_SolveSim::DataSet     DataSet;

    //! Simulation input/output, shared with US_SolveSim
    typedef US_SolveSim::Simulation  Simulation;

    //! Constructor for the multi-data-set Solve-Simulation class
    //!
    //! \param data_sets      The set of data sets for which to solve
    //! \param thrnrank       Thread number or processor rank (1,...)
    //! \param signal_wanted  Flag whether to emit progress signals
    US_SolveSimMDS     ( QList< DataSet* >&, int, bool = false );

  public slots:

    //! \brief Check if implied grid size is beyond limits
    //! \param s_max     S-value maximum
    //! \param smsg      Returned size error message (if return=true)
    //! \returns         Flag of size problem existing
    bool check_grid_size( double, QString& );

    //! \brief Calculate a simulation and the resulting residuals
    //!
    //! On return, \c sim_vals.ti_noise holds one block of \c pointCount()
    //! values per data set and \c sim_vals.ri_noise one block of
    //! \c scanCount() values per data set, in data-set offset order.
    //!
    //! \param offset         Starting data-sets offset
    //! \param dataset_count  Number of data sets for which to solve
    //! \param sim_vals       Simulation parameters object
    //! \param padAB          Optional flag to pad saved A and B
    //! \param ASave          Optional pointer for saving A matrix
    //! \param BSave          Optional pointer for saving B matrix
    //! \param NSave          Optional pointer for saving norm vector
    void calc_residuals( int, int, Simulation&, bool = false,
                         QVector< double >* = 0, QVector< double >*  = 0,
                         QVector< double >* = 0 );

    //! \brief Set a flag so that the worker aborts at the earliest opportunity
    void abort_work    ( void );

  signals:
    //! \brief emit a signal that includes a progress step count
    void work_progress ( int );

  private:

    enum attr_type { ATTR_S, ATTR_K, ATTR_W, ATTR_V, ATTR_D, ATTR_F };

    //! \brief Where one data set's block sits in each concatenated vector
    //!
    //! The fit works on vectors that run over the whole series.  "B" and each
    //! column of "A" are scan-major within a data set and data-set-major
    //! overall; the TI vectors carry one value per radial point of each set
    //! and the RI vectors one value per scan.  A data set's three offsets are
    //! therefore different numbers, and every noise routine needs all three.
    class DsGeom
    {
       public:
          int npoints;  //!< Radial points in this data set
          int nscans;   //!< Scans in this data set
          int toffs;    //!< Offset of the block in B, or in an A column
          int tioffs;   //!< Offset of the block in a TI-length vector
          int rioffs;   //!< Offset of the block in an RI-length vector
    };

    QList< DataSet* >& data_sets;     // Data sets for which to solve

    int                thrnrank;      // Thread number or processor rank (1,...)
    bool               signal_wanted; // Flag whether to emit progress signals

    int                d_offs;        // Current data offset
    int                noisflag;      // Calc-noise flag (0-3 for no|ti|ri|both)
    int                dbg_level;     // Debug level
    bool               dbg_timing;    // Flag whether to print timings
    bool               abort;         // Flag to abort at next opportunity
    bool               calc_ti;       // Calculate-TI-noise flag
    bool               calc_ri;       // Calculate-RI-noise flag
    bool               banddthr;      // Band-forming data threshold peak enhance
    QDateTime          startCalc;     // Start calc time for elapsed time prints

    QVector< DsGeom >  dsgeom;        // Per-data-set block geometry
    int                nt_total;      // Total points, all data sets
    int                nti_total;     // Total TI noise values, all data sets
    int                nri_total;     // Total RI noise values, all data sets
    int                na_stride;     // Row count of one "A" column

  private slots:
    // Build the per-data-set geometry table used by the noise routines
    void build_geometry    ( int, int );

    // Compute "a~", the average experiment signal at each time
    void compute_a_tilde   ( QVector< double >&, const QVector< double >& );

    // Compute "L~s", the average signal at each radius
    void compute_L_tildes  ( int, int,
                                          QVector< double >&,
                                          const QVector< double >& );

    // Compute "L~", the average model signal at each radius
    void compute_L_tilde   ( QVector< double >&,
                                          const QVector< double >& );

    // Compute "L"
    void compute_L         ( int, int,
                                          QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >& );

    // Compute "small_a" and "small_b" matrices for RI noise
    void ri_small_a_and_b  ( int, int, int,
                                          QVector< double >&,
                                          QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >& );

    // Compute "small_a" and "small_b" matrices for TI noise
    void ti_small_a_and_b  ( int, int, int,
                                          QVector< double >&,
                                          QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >& );

    // Compute "L_bar"
    void compute_L_bar     ( QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >& );

    // Compute "a_bar"
    void compute_a_bar     ( QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >& );

    // Compute "L_bar-s"
    void compute_L_bars    ( int, int, int, int,
                                         QVector< double >&,
                                          const QVector< double >&,
                                          const QVector< double >& );

    // Limit data to thresholds
    bool data_threshold    ( US_DataIO::RawData*,
                             double, double, double, double );

    // Limit data to thresholds  (experiment data version)
    bool data_threshold    ( US_DataIO::EditedData*,
                             double, double, double, double );

    // Set a model component attribute value
    void set_comp_attr     ( US_Model::SimulationComponent&,
                             US_Solute&, int );

    // Output a debug print of time for a labelled event
    void DebugTime         ( QString );

    double angle_vectors( QVector<double>&, QVector<double>&, int );
    double angle_vectors( double*, double*, int );
};
#endif
