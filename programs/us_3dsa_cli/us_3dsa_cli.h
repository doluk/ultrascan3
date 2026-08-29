//! \file us_3dsa_cli.h
#ifndef US_3DSA_CLI_H
#define US_3DSA_CLI_H

#include <QtCore>

#include "us_3dsa_process.h"
#include "us_solve_sim.h"

//! \brief Headless driver for the 3DSA fit engine
//!
//! Two subcommands:
//!
//! \li \c fit       -- fit a buoyancy-contrast series described by a JSON
//!                     case file, and report the global and per-data-set
//!                     RMSD.
//! \li \c gen-cases -- write the test-case tree the harness drives: model,
//!                     buffer and simulation-parameter XML for every data
//!                     set, plus a manifest naming the us_astfem_sim runs
//!                     that produce the data.
//!
//! Everything here links only against usutils, so the tool builds in the
//! headless HPC profile as well as the full application build.
class US_3dsaCli
{
   public:

      //! One species of a synthetic truth model
      struct Species
      {
         double s20w;    //!< Sedimentation coefficient, 20W
         double ff0;     //!< Frictional ratio
         double vbar20;  //!< Partial specific volume, 20W
         double conc;    //!< Signal concentration
      };

      //! One data set of a series
      struct DataSetSpec
      {
         QString auc;          //!< Path to the .auc file
         QString simparams;    //!< Path to the simulation-parameter XML
         QString label;        //!< Human-readable name
         double  d2o_percent;  //!< D2O volume percent, for reporting
         double  density;      //!< Buffer density at 20 C
         double  viscosity;    //!< Buffer viscosity at 20 C
         double  temperature;  //!< Run temperature
         double  loading;      //!< Loading relative to the first data set
         double  vbar20;       //!< Nominal vbar for the corrections

         //! The noise this cell was generated with, in OD.  A series need not
         //! carry the same noise in every cell -- different exposure, different
         //! windows, a different day -- so these are per data set, and the
         //! aggregate RMSD checks cannot stand in for them.
         double  rnoise  = 0.0;
         double  tinoise = 0.0;
         double  rinoise = 0.0;

         //! Cap on this data set's own residual RMSD.  0 falls back to the
         //! case-wide cap.  A series whose cells carry different noise has no
         //! single meaningful cap and no meaningful spread, so it sets these
         //! instead.
         double  rmsd_max = 0.0;

         //! Where us_astfem_sim wrote this data set's run, and the run id it
         //! used.  The 2DSA cross-check (see the harness README) needs both:
         //! the edit XML and the experiment XML live there, and the solution
         //! and analyte the simulator registered are found through them.
         QString run_dir;
         QString run_id;
      };

      //! A complete case
      struct Case
      {
         QString                 name;
         QString                 description;
         QVector< Species >      species;
         QVector< DataSetSpec >  datasets;
         double                  rnoise = 0.0;   // random noise, OD
         double                  tinoise = 0.0;  // time-invariant noise, OD
         double                  rinoise = 0.0;  // radially-invariant, OD

         // Grid
         double s_min = 1.0e-13, s_max = 10.0e-13;   int s_res = 24;
         double k_min = 1.0,     k_max = 4.0;        int k_res = 16;
         double v_min = 0.60,    v_max = 0.85;       int v_res = 11;
         int    grid_reps = 1;

         //! Radial margin, in cm, trimmed off the meniscus end of the raw
         //! data to make an edited range strictly inside the cell.
         //!
         //! US_AstfemMath's interpolation exits the process outright when the
         //! simulation grid does not reach the first edited radius, and the
         //! simulator's own default of meniscus + 0.0005 cm is inside the
         //! rounding of the radial grid.  The same margin is written into the
         //! edit XML so that us_2dsa analyses exactly the radii the 3DSA fit
         //! used.
         double edit_margin = 0.02;

         //! Radial margin trimmed off the bottom end.  Larger than the top
         //! one: this is where the boundary piles up, and it matches what
         //! us_astfem_sim writes into its own edit profile.
         double edit_bottom_margin = 0.10;

         //! Partial specific volume to plant in the analyte and solution
         //! records the simulator registers, in place of the truth.
         //!
         //! us_astfem_sim writes the data with an edit profile, a buffer, a
         //! solution and one analyte per species, so the run loads as though
         //! it had come through us_convert and us_edit.  Those analytes carry
         //! the v̄ the data was generated from, which would hand a 2DSA
         //! analysis the answer 3DSA is being asked to find.  The harness
         //! rewrites them to this value, which is what a real analyst would
         //! have: a plausible guess, not the truth.
         double decoy_vbar = 0.0;

         // Fit
         int    threads         = 4;
         int    noisflag        = 0;
         bool   fit_scales      = true;
         bool   ignore_contrast = false;

         // Expectations
         double vbar_tol    = 0.02;
         double s_tol       = 0.5e-13;
         bool   must_refuse = false;

         //! Cap on the global RMSD.  For a case with no noise this is a
         //! fit-quality check; for a case whose noise the fit is asked to
         //! solve for, it is the check that the noise was actually removed.
         double rmsd_max = 0.0;          // 0 disables

         //! Cap on the ratio of the largest per-data-set RMSD to the
         //! smallest.  A fit that cleans up one data set and leaves the rest
         //! alone passes every aggregate check but fails this one.
         double rmsd_spread_max = 0.0;   // 0 disables
      };

      static int main( int argc, char** argv );

   private:

      static int  run_fit      ( const QStringList& );
      static int  run_gen_cases( const QStringList& );
      static void usage        ( void );

      static bool read_case ( const QString&, Case&, QString& );
      static void write_case( const QString&, const Case& );

      // Build the engine inputs from a case
      static bool load_datasets( const Case&,
                                 QList< US_SolveSim::DataSet* >&, QString& );

      // Concentration-weighted attribute of a model
      static double weighted( const US_Model&, int attr );

      // The truth values a case was generated from
      static double truth_vbar( const Case& );
      static double truth_s   ( const Case& );
};
#endif
