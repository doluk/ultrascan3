//! \file us_fvm_gradients.h
#ifndef US_FVM_GRADIENTS_H
#define US_FVM_GRADIENTS_H

#include <QtCore>

#include "us_buffer.h"
#include "us_constants.h"
#include "us_dataIO.h"
#include "us_extern.h"
#include "us_lamm_astfvm.h"
#include "us_math_bf.h"
#include "us_simparms.h"

//! \brief The density gradients a finite-volume (ASTFVM) simulation of one
//!        data set needs, prepared once and reused for every solute
//!
//! A buffer whose components sediment (co-sedimenting: CsCl, Nycodenz) forms
//! a density and viscosity gradient as the run proceeds.  So does a
//! band-forming cell whose overlay and buffer differ in a component that
//! diffuses but does not sediment (co-diffusing: sucrose, D2O).  A solute's
//! local s then depends on its own vbar through the local density, which is
//! what lets a single run determine vbar.
//!
//! US_LammAstfvm does not build these gradients on its own: the co-sedimenting
//! one needs the experiment's grid, which set_buffer() does not have, and its
//! internal fallback discards what it computes.  A caller that simulates many
//! solutes against one data set -- a 2DSA or 3DSA fit -- prepares them here,
//! once, and passes them to every US_LammAstfvm::set_buffer().  The
//! preparation follows US_2dsaProcess on the cosedimenting branch, except
//! that the former is simulated by the ASTFVM rather than ASTFEM: the same
//! cell and clock as the solutes it carries, where ASTFEM puts the bottom at
//! the centerpiece's and runs an acceleration zone as a straight ramp.
//!
//! An object owns its gradients and the component list they refer to.  The
//! gradients keep working state while a simulation reads them, so each
//! thread needs its own: copy-construct one per thread.
class US_UTIL_EXTERN US_FvmGradients
{
   public:
      US_FvmGradients();

      //! \brief A deep copy, for another thread
      US_FvmGradients( const US_FvmGradients& );

      US_FvmGradients&          operator=( const US_FvmGradients& ) = delete;

      //! \brief Whether a buffer's components form a gradient during a run:
      //!        any component that sediments or diffuses on its own
      static bool               forms_gradient( const US_Buffer& );

      //! \brief Prepare the gradients for one data set
      //!
      //! A co-sedimenting gradient is an ASTFVM simulation of each
      //! gradient-forming component as one ideal solute, from the uniform
      //! loading at time zero to the grid's last scan; a co-diffusing one is
      //! the band-forming solution of US_Math_BF.  A buffer that forms
      //! neither prepares nothing and succeeds.
      //! \param buffer  The data set's buffer
      //! \param sparms  Its simulation parameters, with the timestate loaded
      //! \param grid    Its experiment grid: radii and scan times
      //! \param msg     Returned description of a failure
      //! \returns       True on success
      bool                      prepare( const US_Buffer&, const US_SimulationParameters&, const US_DataIO::RawData&, QString& );

      //! \brief Take a measured gradient instead of simulating one
      //!
      //! For a gradient former the model describes poorly -- a concentrated,
      //! non-ideal one such as sucrose or CsCl at the loads ABDE uses --
      //! when the run measured it, by interference say.  The buffer must
      //! list the former as a co-sedimenting component (non-zero s), whose
      //! density and viscosity coefficients turn its concentration into
      //! the solution's; its s and D are not used.
      //! \param buffer    The data set's buffer
      //! \param former    Name of the gradient-forming component
      //! \param molar     Its molar concentration, meniscus to bottom, one
      //!                   scan per time, from the start of the run
      //! \param msg       Returned description of a failure
      //! \returns         True on success
      bool                      prepare_measured( const US_Buffer&, const QString&, const US_DataIO::RawData&, QString& );

      //! \brief Whether the gradient was measured rather than simulated
      bool                      is_measured( void ) const
      {
         return measured_;
      }

      //! \brief The co-sedimenting gradient, or null
      US_LammAstfvm::CosedData* cosed( void )
      {
         return cosed_.data();
      }

      //! \brief The band-forming (co-diffusing) gradient, or null
      US_Math_BF::Band_Forming_Gradient* band_forming( void )
      {
         return bfg_.data();
      }

      //! \brief Whether anything was prepared
      bool is_gradient( void ) const
      {
         return !cosed_.isNull() || !bfg_.isNull();
      }

      //! \brief Lowest density in the cell over the run, g/mL
      double density_lo( void ) const
      {
         return dens_lo;
      }

      //! \brief Highest density in the cell over the run, g/mL
      double density_hi( void ) const
      {
         return dens_hi;
      }

      //! \brief The grid to prepare gradients on
      //!
      //! The gradient has to cover the whole cell, meniscus to bottom, at
      //! times spread over the run: an edit may keep a single (equilibrium)
      //! scan, and the solver interpolates the gradient between the scans it
      //! was prepared on.  rpm and omega-squared-t come from the loaded
      //! timestate.
      //! \param sparms   Simulation parameters, timestate loaded
      //! \param t_end    Time of the last scan to be simulated, s
      //! \param nscans   Number of times to prepare the gradient at
      //! \param dr       Radial spacing, cm
      static US_DataIO::RawData gradient_grid( const US_SimulationParameters&, double, int = 100, double = 0.002 );

      //! \brief The density at given radii and time, g/mL, as the solver
      //!        forms it: the buffer's own, plus each gradient's increment.
      //!        Kept per time, since a fit asks for the same few scans over
      //!        and over; the radii must be the same at each call.
      //! \param t      Time, s
      //! \param temp   Temperature, C
      //! \param x      Radii, cm
      //! \returns      Density at each radius
      const QVector<double>&    density_at( double, double, const QVector<double>& );

      //! \brief Bytes identifying the gradient-forming conditions, for
      //!        simulation-cache keys: two data sets with the same grid but
      //!        different buffers must not share simulations
      static QByteArray         key( const US_Buffer&, const US_SimulationParameters& );

      //! \brief A gradient former's s and D fitted to interference scans, and
      //!        what the fit needs
      struct FormerFit
      {
         // In
         US_DataIO::RawData      scans;                   //!< Interference scans, fringes
         US_SimulationParameters sparms;                  //!< Timestate loaded; the scans' meniscus and bottom
         double                  molar             = 0.0; //!< Loading, mol/L
         double                  fringes_per_molar = 0.0; //!< Fringes per mol/L of the former
         double                  vbar              = 0.63;
         double                  base_density      = DENS_20W;
         double                  base_viscosity    = VISC_20W;
         double                  margin            = 0.03; //!< Left out at the meniscus and bottom, cm
         int                     scan_stride       = 1;    //!< Every Nth scan
         int                     radial_stride     = 1;    //!< Every Nth radius
         bool                    concentration_dependent = false; //!< Fit sigma and delta too
         // In as the start, out as the result
         double                  s     = 1.0e-14; //!< s20,w, s
         double                  D     = 2.5e-6;  //!< D20,w, cm^2/s
         double                  sigma = 0.0;     //!< s / ( 1 + sigma c ), L/mol
         double                  delta = 0.0;     //!< D / ( 1 + delta c ), L/mol
         // Out
         double                  rmsd        = 0.0; //!< Fringes, time- and radially invariant noise removed
         int                     evaluations = 0;
         US_DataIO::RawData      residuals;         //!< On the scans and radii fitted
      };

      //! \brief Fit a former's s and D to interference scans
      //!
      //! The former is simulated as in prepare(): the ASTFVM, from a uniform
      //! loading, on the run's own clock, in the base buffer; its molar
      //! concentration times fringes_per_molar is the model.  Interference
      //! scans carry an arbitrary offset per scan and the cell's optical
      //! baseline, so both are removed exactly (radially and time-invariant
      //! noise) before the RMSD is taken.  The amplitude is not fitted: it
      //! is set by the loading and dn/dc, and with it free the gradient's
      //! amplitude and steepness cannot be told apart.  Nelder-Mead over
      //! ln s, ln D (and the square roots of sigma and delta).
      //! \param fit  Inputs, start values; returns the result
      //! \param msg  Returned description of a failure
      //! \returns    True on success
      static bool               fit_former( FormerFit&, QString& );

   private:
      // Lowest and highest density over the cell at the grid's scan times
      void measure_span( const US_DataIO::RawData& );

      // Density and viscosity at radii and a time, from the buffer's own
      void fill_density( double t, double temp, const QVector<double>& x, QVector<double>& dens, QVector<double>& visc );

      QList<US_CosedComponent>                          components; // What cosed_ points at
      QScopedPointer<US_LammAstfvm::CosedData>          cosed_;
      QScopedPointer<US_Math_BF::Band_Forming_Gradient> bfg_;
      double                                            base_density;
      double                                            base_viscosity;
      double                                            dens_lo;
      double                                            dens_hi;
      double                                            buf_density; // The buffer's own
      double                                            buf_viscosity;
      QHash<qint64, QVector<double>>                    density_cache; // By time, ms
      bool                                              measured_ = false;
};
#endif
