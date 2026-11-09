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
//! Transport closure (cross-section averaged, 1-D).  Sedimentation against
//! a non-radial wall forms a depleted and an enriched wall layer whose
//! lateral density difference drives a buoyant overturning flow.  2D
//! Stokes-Boussinesq-Lamm simulations (scripts/misalignment, README sec. 5)
//! show that in the lateral (iso-radius) mean this flow acts as a
//! dispersion of the boundary with a weak forward drift, both linear in the
//! wall tilt psi and only weakly dependent on the loading concentration c:
//! \code
//!   D_eff = D ( 1 + eps_D ),   eps_D = gamma_D (psi/1 deg) (c/1 mg/mL)^-0.12
//!   s_eff = s ( 1 + eps_s ),   eps_s = gamma_s (psi/1 deg) (c/1 mg/mL)^-0.12
//! \endcode
//! Default calibration (BSA, 50 krpm, 0.9 cm column, 0.01-1 mg/mL):
//! gamma_D = 0.043, gamma_s = gamma_D / 12.5.  The convection is driven by
//! the total concentration, so c is the total loading concentration; in a
//! mixture the species are coupled (the dimer of BSA drifts backwards),
//! which this per-species closure does not describe.
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
      //! \param beta_deg   Polar offset of the channel centre line from the
      //!                   cell centre line (degrees; 0 for the centre)
      US_CellMisalignment( double delta_deg = 0.0, double sector_deg = 2.5,
                           double pivot = 6.5, double beta_deg = 0.0 );

      //! \brief True if the misalignment changes the transport (delta and
      //!        at least one closure coefficient non-zero)
      bool   active        () const;

      //! \brief Effective wall tilt (degrees) of the closure: angle of the
      //!        mean wall line with the radius through the pivot
      double tilt          () const;

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

      //! \brief Relative change eps_D of the diffusion coefficient
      double diffusion_eps    () const;

      //! \brief Relative change eps_s of the sedimentation coefficient
      double sedimentation_eps() const;

      double delta_deg;   //!< Rotation of the cell (degrees)
      double sector_deg;  //!< Full sector angle (degrees)
      double pivot;       //!< Pivot radius (cm)
      double beta_deg;    //!< Polar offset of the channel (degrees)
      double gamma_D;     //!< eps_D per degree of tilt at 1 mg/mL
      double gamma_s;     //!< eps_s per degree of tilt at 1 mg/mL
      double conc;        //!< Total loading concentration (mg/mL)
      double conc_exp;    //!< Exponent of the concentration dependence
};

#endif
