//! \file us_grid_control.h
#ifndef US_GRID_CONTROL_H
#define US_GRID_CONTROL_H

#include "us_extern.h"

#include <QtCore>

//! \brief Feature aware selection of temporal and spatial grid resolution.
//!
//! Historically both the time step and the radial grid of the UltraScan Lamm
//! equation solvers are derived from the sedimentation coefficient alone: the
//! time a particle needs to travel from meniscus to bottom along its
//! characteristic line is divided by the requested number of simulation
//! points. That is a reasonable choice for a boundary which starts out as a
//! single step at the meniscus and spreads with the square root of time, but
//! it ignores everything the initial condition and the back coupled
//! (cosedimenting, codiffusing, concentration dependent) effects add on top of
//! it. A band forming experiment for example starts with a lamella that is one
//! to two orders of magnitude narrower than the solution column. The purely
//! characteristic based step then transports that lamella across many cells
//! per step, and the Crank-Nicolson time integration answers with ringing.
//! Switching on a non-ideal case happens to push dt far enough down that the
//! oscillations disappear, which is why the cosedimenting case "just works" -
//! but that is a side effect, not a resolution criterion.
//!
//! This class collects the small amount of arithmetic needed to pick dt and
//! the cell size h *together*, so that
//! - the steepest feature actually present is resolved by a handful of cells,
//! - the front does not travel more than a fraction of a cell per step
//!   (Courant condition),
//! - the diffusion number \f$D\,\Delta t/h^2\f$ stays in the range where a
//!   Crank-Nicolson step damps instead of oscillates,
//! - refinement stops where the time step can no longer support it, so no
//!   cells are spent on a scale that only produces ringing,
//! - and the resolution relaxes back towards the cheap characteristic based
//!   values as soon as the profile is smooth again.
//!
//! All functions are static and free of solver state, so they can be shared by
//! the ASTFEM (us_astfem_rsa) and ASTFVM (us_lamm_astfvm) solvers and unit
//! tested on their own.
class US_UTIL_EXTERN US_GridControl
{
   public:

      //! \brief Tunable parameters of the resolution controller.
      class US_UTIL_EXTERN Config
      {
         public:
            //! \brief Construct a configuration holding the default tuning
            Config();

            bool   enabled;         //!< False restores the legacy, purely
                                    //!< characteristic driven resolution
            double courant;         //!< Largest fraction of a cell the front
                                    //!< may cross within one time step
            double diff_number;     //!< Largest tolerated \f$D\Delta t/h^2\f$
            int    feature_points;  //!< Cells wanted across the steepest
                                    //!< feature of the profile
            double max_reduction;   //!< dt is never pushed below the
                                    //!< characteristic dt divided by this
            double max_refinement;  //!< h is never pushed below the base cell
                                    //!< size divided by this
            double growth;          //!< Largest per step growth factor of dt;
                                    //!< shrinking is always immediate
      };

      //! \brief Default configuration, honoring the debug text overrides
      //!
      //! The debug text key "NO_ADAPTIVE_GRID" disables the controller
      //! completely and restores the legacy behavior. "COARSE_ADAPTIVE_GRID"
      //! keeps it enabled but halves the resolution targets, which is useful
      //! to check whether a feature is resolution limited.
      //! \returns A configuration filled with the active tuning
      static Config config( void );

      //! \brief Width of the lamella of a band forming centerpiece
      //! \param meniscus    Meniscus position in cm
      //! \param band_volume Loaded lamella volume in ml
      //! \param path_length Centerpiece path length in cm
      //! \param angle       Centerpiece sector angle in degrees
      //! \returns           Radial width of the lamella in cm; 0 if the
      //!                    geometry is not usable
      static double lamella_width( double meniscus, double band_volume,
                                   double path_length, double angle );

      //! \brief Length scale of the steepest feature of a profile
      //!
      //! The scale is the total value range divided by the largest slope, i.e.
      //! the radial distance the profile would need to cover its full range at
      //! its steepest point. A flat or linear profile therefore reports the
      //! full column length ("nothing to resolve"), while a sharp front
      //! reports its thickness.
      //! \param x      Radial grid, strictly increasing, nv values
      //! \param nv     Number of grid points
      //! \param u      Profile values; u[ j * stride ] belongs to x[ j ]
      //! \param stride Distance between consecutive node values in u
      //! \returns      Length scale in cm; infinity for a constant profile
      static double front_scale( const double* x, int nv, const double* u,
                                 int stride = 1 );

      //! \brief Smallest spacing of a radial grid
      //! \param x  Radial grid, strictly increasing, nv values
      //! \param nv Number of grid points
      //! \returns  Smallest distance between neighboring points in cm
      static double min_spacing( const double* x, int nv );

      //! \brief Cell size wanted to resolve a feature of the given width
      //!
      //! A feature of zero means a genuine discontinuity, for which the finest
      //! admissible cell is returned; a non-finite feature means there is
      //! nothing to resolve and the unrefined cell size is returned.
      //! \param feature Width of the steepest feature in cm
      //! \param span    Length of the solution column in cm
      //! \param base_h  Cell size of the unrefined grid in cm
      //! \param cfg     Active controller configuration
      //! \returns       Target cell size in cm
      static double target_cell( double feature, double span, double base_h,
                                 const Config& cfg );

      //! \brief Time step supported by a given cell size
      //!
      //! Applies the Courant limit of the sedimentation transport and the
      //! diffusion number limit of the Crank-Nicolson step, then clamps the
      //! result so it never falls below dt_char / Config::max_reduction.
      //! \param dt_char   Characteristic (sedimentation driven) time step in s
      //! \param h         Cell size to support in cm
      //! \param velocity  Largest radial front velocity in cm/s
      //! \param diffusion Largest diffusion coefficient in cm^2/s
      //! \param cfg       Active controller configuration
      //! \returns         Admissible time step in s
      static double target_step( double dt_char, double h, double velocity,
                                 double diffusion, const Config& cfg );

      //! \brief Time step for a profile whose steepest feature is known
      //!
      //! This is the single entry point the solvers use. It picks the cell
      //! size wanted for the feature, refuses to go finer than the cheapest
      //! admissible step could carry, and returns the characteristic step
      //! unchanged whenever the feature is already resolved by the grid the
      //! user asked for through simpoints. Smooth simulations therefore keep
      //! exactly the performance they had.
      //! \param dt_char   Characteristic (sedimentation driven) time step in s
      //! \param feature   Width of the steepest feature in cm
      //! \param span      Length of the solution column in cm
      //! \param base_h    Cell size of the unrefined grid in cm
      //! \param velocity  Largest radial front velocity in cm/s
      //! \param diffusion Largest diffusion coefficient in cm^2/s
      //! \param cfg       Active controller configuration
      //! \returns         Time step to aim for in s
      static double step_for_feature( double dt_char, double feature,
                                      double span, double base_h,
                                      double velocity, double diffusion,
                                      const Config& cfg );

      //! \brief Smallest cell size a given time step can support
      //!
      //! This is the inverse of target_step() and is used to stop the mesh
      //! refinement where extra cells would only add ringing.
      //! \param dt        Time step in s
      //! \param velocity  Largest radial front velocity in cm/s
      //! \param diffusion Largest diffusion coefficient in cm^2/s
      //! \param cfg       Active controller configuration
      //! \returns         Smallest useful cell size in cm
      static double min_cell( double dt, double velocity, double diffusion,
                              const Config& cfg );

      //! \brief Relax a time step towards its new target
      //!
      //! Shrinking is applied at once, because a feature that just sharpened
      //! must be resolved from the very next step on. Growing is limited to
      //! Config::growth per step so that the solver does not oscillate between
      //! a fine and a coarse resolution.
      //! \param dt_prev   Time step used in the previous step in s
      //! \param dt_target Time step wanted for the next step in s
      //! \param cfg       Active controller configuration
      //! \returns         Time step to use for the next step in s
      static double relax( double dt_prev, double dt_target,
                           const Config& cfg );

      //! \brief Radial front velocity of a sedimenting species
      //! \param s      Sedimentation coefficient in s
      //! \param omega2 Squared angular velocity in 1/s^2
      //! \param radius Radial position in cm
      //! \returns      Absolute radial velocity in cm/s
      static double front_velocity( double s, double omega2, double radius );

      //! \brief Number of grid points needed to cover a span with a cell size
      //! \param span Length of the region in cm
      //! \param h    Wanted cell size in cm
      //! \returns    Number of points, at least 2
      static int    points_for_span( double span, double h );
};
#endif
