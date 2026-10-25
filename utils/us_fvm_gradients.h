//! \file us_fvm_gradients.h
#ifndef US_FVM_GRADIENTS_H
#define US_FVM_GRADIENTS_H

#include <QtCore>

#include "us_buffer.h"
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
//! preparation follows US_2dsaProcess on the cosedimenting branch step for
//! step.
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
      //! A co-sedimenting gradient is a simulation of the gradient-forming
      //! components themselves over the experiment's scans; a co-diffusing
      //! one is the band-forming solution of US_Math_BF.  A buffer that forms
      //! neither prepares nothing and succeeds.
      //! \param buffer  The data set's buffer
      //! \param sparms  Its simulation parameters, with the timestate loaded
      //! \param grid    Its experiment grid: radii and scan times
      //! \param msg     Returned description of a failure
      //! \returns       True on success
      bool                      prepare( const US_Buffer&, const US_SimulationParameters&, const US_DataIO::RawData&, QString& );

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
};
#endif
