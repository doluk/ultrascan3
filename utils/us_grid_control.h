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
//! it says nothing about how steep the solution actually is. A profile that
//! carries structure narrower than the characteristic assumes gets transported
//! across many cells per step, and the Crank-Nicolson time integration answers
//! with ringing. Switching on a non-ideal case happens to push dt far enough
//! down that the oscillations disappear, which is why the cosedimenting case
//! "just works" - but that is a side effect, not a resolution criterion.
//!
//! This class derives dt and the cell size from one measured quantity: the
//! width of the steepest feature the profile currently has. Nothing here knows
//! what produced that feature - a layered band, a hand written initial
//! concentration vector, the first scan of an experiment, a self sharpening
//! boundary, a cosedimenting density gradient - nor where in the cell it sits,
//! nor which way it travels. Floating species (s < 0) and mixtures of floating
//! and sedimenting species are covered by the same arithmetic, because only
//! the magnitude of the transport velocity enters.
//!
//! The two limits are:
//! - transport: a feature must not be carried across more than a fraction of
//!   its own width in one step,
//! - diffusion: the Crank-Nicolson amplification factor at the wavenumber the
//!   feature occupies, \f$k=2\pi/\delta\f$, must stay positive, which means
//!   \f$D\,\Delta t\,k^2<1\f$, i.e. \f$\Delta t < \delta^2/(4\pi^2 D)\f$.
//!   Beyond that bound the content at the feature's own scale is inverted
//!   rather than damped from step to step, and that is what is seen as
//!   oscillations.
//!
//! Both bounds grow with the square of the feature width, so a front that
//! spreads diffusively releases the resolution again all by itself: since
//! \f$\delta^2 \sim 2Dt\f$, the admissible step grows in proportion to the
//! elapsed time and the whole early phase costs only a logarithmic number of
//! extra steps. A profile with no steep structure at all is left with the
//! classic characteristic step.
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
            double courant;         //!< Largest fraction of its own width a
                                    //!< feature may be transported per step
            double ring_factor;     //!< Diffusive budget at the feature scale:
                                    //!< dt <= ring_factor * width^2 / D. The
                                    //!< default 1/(4*pi^2) is the point where
                                    //!< a Crank-Nicolson step stops damping
                                    //!< and starts inverting that scale
            int    feature_points;  //!< Cells wanted across the narrowest
                                    //!< feature a step can still carry
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
      //! keeps it enabled but relaxes the resolution targets, which is useful
      //! to check whether a feature is resolution limited.
      //! \returns A configuration filled with the active tuning
      static Config config( void );

      //! \brief Length scale of the steepest feature of a profile
      //!
      //! The scale is the total value range divided by the largest slope, i.e.
      //! the radial distance the profile would need to cover its full range at
      //! its steepest point. A flat or linear profile therefore reports the
      //! full column length ("nothing to resolve"), while a sharp front
      //! reports its thickness. Where the profile carries several features the
      //! narrowest one wins, which is what the time step has to be sized for.
      //!
      //! A feature narrower than the grid can only be reported as about one
      //! cell wide. That underestimates the true width and therefore errs on
      //! the safe side, and it corrects itself as the grid is refined.
      //! \param x      Radial grid, strictly increasing, nv values
      //! \param nv     Number of grid points
      //! \param u      Profile values; u[ j * stride ] belongs to x[ j ]
      //! \param stride Distance between consecutive node values in u
      //! \returns      Length scale in cm; infinity for a constant profile
      static double front_scale( const double* x, int nv, const double* u,
                                 int stride = 1 );

      //! \brief Magnitude of a profile, used to normalize steepness measures
      //! \param u      Profile values; u[ j * stride ] belongs to node j
      //! \param nv     Number of grid points
      //! \param stride Distance between consecutive node values in u
      //! \returns      Value range of the profile; 0 for a constant one
      static double magnitude( const double* u, int nv, int stride = 1 );

      //! \brief Smallest spacing of a radial grid
      //! \param x  Radial grid, strictly increasing, nv values
      //! \param nv Number of grid points
      //! \returns  Smallest distance between neighboring points in cm
      static double min_spacing( const double* x, int nv );

      //! \brief Time step admissible for a feature of the given width
      //!
      //! Applies the transport and diffusive-ringing bounds and clamps the
      //! result so it never falls below dt_char / Config::max_reduction. A
      //! non-finite width means there is no feature and the characteristic
      //! step is returned unchanged; a width of exactly zero means a genuine
      //! discontinuity, which no step can resolve, so the floor is returned.
      //! \param dt_char   Characteristic (sedimentation driven) time step in s
      //! \param feature   Width of the steepest feature in cm
      //! \param velocity  Largest radial transport speed in cm/s (magnitude,
      //!                  so the sign of s does not matter)
      //! \param diffusion Largest diffusion coefficient in cm^2/s
      //! \param cfg       Active controller configuration
      //! \returns         Admissible time step in s
      static double step_for_feature( double dt_char, double feature,
                                      double velocity, double diffusion,
                                      const Config& cfg );

      //! \brief Narrowest feature a given time step can still carry
      //! \param dt        Time step in s
      //! \param velocity  Largest radial transport speed in cm/s
      //! \param diffusion Largest diffusion coefficient in cm^2/s
      //! \param cfg       Active controller configuration
      //! \returns         Feature width in cm; 0 when unconstrained
      static double resolvable_feature( double dt, double velocity,
                                        double diffusion, const Config& cfg );

      //! \brief Smallest cell size worth creating at a given time step
      //!
      //! Cells below this size subdivide a feature the step cannot carry in
      //! the first place, so they add cost and ringing but no accuracy. Used
      //! as the floor of the mesh refinement.
      //! \param dt        Time step in s
      //! \param velocity  Largest radial transport speed in cm/s
      //! \param diffusion Largest diffusion coefficient in cm^2/s
      //! \param cfg       Active controller configuration
      //! \returns         Smallest useful cell size in cm; 0 when unconstrained
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

      //! \brief Radial transport speed of a sedimenting or floating species
      //! \param s      Sedimentation coefficient in s (either sign)
      //! \param omega2 Squared angular velocity in 1/s^2
      //! \param radius Radial position in cm
      //! \returns      Absolute radial speed in cm/s
      static double front_velocity( double s, double omega2, double radius );

      //! \brief Radial intervals a grid does not resolve well enough
      //!
      //! Walks the profile and marks every interval whose own length scale,
      //! spread over Config::feature_points cells, would not fit into the
      //! spacing the grid has there. Neighboring marks are merged and every
      //! region is padded, so a feature keeps its resolution while it travels
      //! and spreads. Makes no assumption about how many features there are,
      //! where they sit, or which way they move, so a boundary at the bottom
      //! of a floating species is treated exactly like a band at the meniscus.
      //! \param x      Radial grid, strictly increasing, nv values
      //! \param nv     Number of grid points
      //! \param u      Profile values; u[ j * stride ] belongs to x[ j ]
      //! \param stride Distance between consecutive node values in u
      //! \param h      Cell size the marked regions should be resolved with
      //! \param pad    Radial distance to widen every region by, on both sides
      //! \param cfg    Active controller configuration
      //! \param lo     Receives the inner radius of every region
      //! \param hi     Receives the outer radius of every region
      //! \returns      Number of regions found
      static int    steep_regions( const double* x, int nv, const double* u,
                                   int stride, double h, double pad,
                                   const Config& cfg,
                                   QVector< double >& lo,
                                   QVector< double >& hi );

      //! \brief Number of grid points needed to cover a span with a cell size
      //! \param span Length of the region in cm
      //! \param h    Wanted cell size in cm
      //! \returns    Number of points, at least 2
      static int    points_for_span( double span, double h );

      //! \brief Width of the lamella of a band forming centerpiece
      //!
      //! Geometry of the initial condition, not of the resolution: the
      //! controller never sees this value, the solvers only use it to build
      //! the profile the controller then measures.
      //! \param meniscus    Meniscus position in cm
      //! \param band_volume Loaded lamella volume in ml
      //! \param path_length Centerpiece path length in cm
      //! \param angle       Centerpiece sector angle in degrees
      //! \returns           Radial width of the lamella in cm; 0 if the
      //!                    geometry is not usable
      static double lamella_width( double meniscus, double band_volume,
                                   double path_length, double angle );
};
#endif
