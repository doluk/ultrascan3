//! \file us_cell_misalign.h
#ifndef US_CELL_MISALIGN_H
#define US_CELL_MISALIGN_H

#include "us_extern.h"

//! \brief Geometry and transport closure for a rotationally misaligned cell
//!
//! The cell (and therefore the sector-shaped channel) is rotated in the rotor
//! hole by an angle delta about a pivot on the channel centre line at radius
//! r_p (normally the cell centre).  The sector walls are then no longer
//! radial: a wall that originally ran along polar angle phi0 = beta +- alpha
//! becomes the straight line of direction phi0 + delta at distance
//! \code
//!   p = r_p [ sin( phi0 + delta ) - sin( phi0 ) ]          (~ r_p delta)
//! \endcode
//! from the rotor axis.  At radius r the wall makes the angle psi with the
//! radial direction, sin(psi) = p / r.  The angular width of the channel
//! changes only at O(alpha delta^2), so the radial cross-section A(r) of the
//! aligned sector is retained.
//!
//! Transport closure (cross-section averaged, 1-D).  At a non-radial wall
//! the wall-normal sedimentation flux c v sin(psi) is balanced by diffusion,
//! which forms a depleted layer on one wall and an enriched layer on the
//! other.  These lateral density anomalies cannot be in hydrostatic
//! equilibrium; the depleted layer rises to its neutral level (the boundary),
//! the enriched layer slides to the bottom (Boycott effect, Ponder-Nakamura-
//! Kuroda kinematics).  The cross-section averaged Lamm equation becomes
//! \code
//!   dc/dt = -(1/r) d/dr [ r ( s w^2 r c - D dc/dr ) ] - u(r) dc/dr
//!   u(r)  = kappa  s w^2  p (b - r) / ( Phi r )
//! \endcode
//! (Phi = angular width of the channel, b = bottom) with the solute removed
//! by the last term deposited at the bottom.  In conservative form, as used
//! by solvers:
//! \code
//!   v_eff(r) = s w^2 r ( 1 + eps(r) ),   eps(r) = kappa p (b - r) / (Phi r^2)
//!   sink     = -k(r) c,                  k(r)   = kappa s w^2 p / (Phi r)
//! \endcode
//! kappa (0..1) is the transport efficiency of the wall layers: 1 is the
//! convection-dominated (PNK) limit, 0 the ideal aligned cell.
//!
//! All angles are in degrees at the interface and radians internally;
//! lengths in cm.
class US_UTIL_EXTERN US_CellMisalignment
{
   public:
      //! \brief Construct with misalignment parameters
      //! \param delta_deg  Rotation of the cell in the rotor hole (degrees)
      //! \param sector_deg Full sector angle of the channel (degrees)
      //! \param pivot      Radius of the rotation pivot (cm), normally the
      //!                   cell centre (6.5 cm for An-50/An-60 rotors)
      //! \param kappa      Wall-layer transport efficiency (0..1)
      //! \param beta_deg   Polar offset of the channel centre line from the
      //!                   cell centre line (degrees; 0 for the centre)
      US_CellMisalignment( double delta_deg = 0.0, double sector_deg = 2.5,
                           double pivot = 6.5, double kappa = 1.0,
                           double beta_deg = 0.0 );

      //! \brief True if the misalignment changes the transport (delta, kappa
      //!        and sector angle all non-zero)
      bool   active        () const;

      //! \brief Distance of a wall line from the rotor axis (cm)
      //! \param side  +1 or -1 for the two walls
      double wall_offset   ( int side ) const;

      //! \brief Mean wall offset |p| used by the closure (cm)
      double mean_offset   () const;

      //! \brief Angle between a wall and the radial direction at r (degrees)
      double wall_tilt     ( double r, int side = 1 ) const;

      //! \brief Polar angle of a wall at radius r (radians)
      double wall_phi      ( double r, int side ) const;

      //! \brief Angular width of the channel at radius r (radians)
      double angular_width ( double r ) const;

      //! \brief Radial displacement of an off-centre channel (cm), first
      //!        order in delta: -delta r_p sin(beta)
      double radial_shift  () const;

      //! \brief Distance (cm) from the optical beam at the nominal channel
      //!        centre line to the nearest wall; negative if clipped
      double beam_margin   ( double r ) const;

      //! \brief Radial extent (cm) spanned by a flat bottom face after
      //!        rotation (channel width x sin delta)
      double bottom_smear  ( double bottom ) const;

      //! \brief Relative enhancement eps(r) of the sedimentation velocity
      //! \param r       Radius (cm)
      //! \param bottom  Bottom radius (cm)
      double velocity_eps  ( double r, double bottom ) const;

      //! \brief First-order sink rate k(r) (1/s) of the conservative form
      //! \param r       Radius (cm)
      //! \param s_w2    s * omega^2 (1/s)
      double sink_rate     ( double r, double s_w2 ) const;

      double delta_deg;   //!< Rotation of the cell (degrees)
      double sector_deg;  //!< Full sector angle (degrees)
      double pivot;       //!< Pivot radius (cm)
      double kappa;       //!< Wall-layer transport efficiency (0..1)
      double beta_deg;    //!< Polar offset of the channel (degrees)
};

#endif
