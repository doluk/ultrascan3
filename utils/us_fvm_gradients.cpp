//! \file us_fvm_gradients.cpp
#include "us_fvm_gradients.h"
#include "us_constants.h"
#include "us_math2.h"

#include <cmath>

US_FvmGradients::US_FvmGradients()
{
   base_density   = 0.0;
   base_viscosity = 0.0;
   dens_lo        = 0.0;
   dens_hi        = 0.0;
}

US_FvmGradients::US_FvmGradients( const US_FvmGradients& other )
{
   components     = other.components;
   base_density   = other.base_density;
   base_viscosity = other.base_viscosity;
   dens_lo        = other.dens_lo;
   dens_hi        = other.dens_hi;

   if ( other.cosed_ )
   {
      cosed_.reset( new US_LammAstfvm::CosedData( *other.cosed_ ) );
      cosed_->cosed_components = &components; // ours, not the original's
   }

   if ( other.bfg_ )
   {
      bfg_.reset( new US_Math_BF::Band_Forming_Gradient( *other.bfg_ ) );
   }
}

bool US_FvmGradients::forms_gradient( const US_Buffer& buffer )
{
   for ( const US_CosedComponent& cc : buffer.cosed_component )
   {
      if ( cc.s_coeff != 0.0 || cc.d_coeff != 0.0 )
      {
         return true;
      }
   }

   return false;
}

QByteArray US_FvmGradients::key( const US_Buffer& buffer, const US_SimulationParameters& sparms )
{
   QByteArray  bytes;
   QDataStream ds( &bytes, QIODevice::WriteOnly );
   ds.setFloatingPointPrecision( QDataStream::DoublePrecision );

   ds << buffer.density << buffer.viscosity << buffer.manual;
   ds << sparms.band_forming << sparms.band_volume << sparms.cp_pathlen << sparms.cp_angle;

   for ( const US_CosedComponent& cc : buffer.cosed_component )
   {
      ds << cc.name << cc.conc << cc.s_coeff << cc.d_coeff << cc.vbar << cc.overlaying;

      for ( int jj = 0; jj < 6; jj++ )
      {
         ds << cc.dens_coeff[jj] << cc.visc_coeff[jj];
      }
   }

   return bytes;
}

US_DataIO::RawData US_FvmGradients::gradient_grid( const US_SimulationParameters& sparms, double t_end, int nscans, double dr )
{
   QVector<double> rpm;
   QVector<double> w2t;

   for ( const US_SimulationParameters::SimSpeedProf& prof : sparms.sim_speed_prof )
   {
      rpm += prof.rpm_timestate;
      w2t += prof.w2t_timestate;
   }

   US_DataIO::RawData grid;
   grid.type[0] = 'R';
   grid.type[1] = 'A';
   grid.cell    = 1;
   grid.channel = 'A';

   const int nrad = qMax( 2, qRound( ( sparms.bottom - sparms.meniscus ) / dr ) + 1 );
   for ( int jj = 0; jj < nrad; jj++ )
   {
      grid.xvalues << sparms.meniscus + ( sparms.bottom - sparms.meniscus ) * jj / ( nrad - 1 );
   }

   const int ntimes = qMin( rpm.size(), w2t.size() );
   nscans           = qMax( 3, nscans );

   // Start once the rotor is at speed: the simulator leaves scans inside the
   // acceleration zone empty, and an empty first scan would read as no
   // gradient at all.  Before the first scan the solver holds that scan's
   // gradient.
   const double t_start = sparms.sim_speed_prof.isEmpty() ? 0.0 : (double) sparms.sim_speed_prof[0].time_e_accel + 1.0;
   const double t_first = qMin( qMax( t_start, t_end / nscans ), t_end );

   for ( int kk = 0; kk < nscans; kk++ )
   {
      US_DataIO::Scan scan;
      scan.seconds     = t_first + ( t_end - t_first ) * kk / ( nscans - 1 );
      const int it     = qBound( 0, qRound( scan.seconds ), ntimes - 1 );
      scan.rpm         = ( ntimes > 0 ) ? rpm[it] : sparms.speed_step[0].rotorspeed;
      scan.omega2t     = ( ntimes > 0 ) ? w2t[it] : 0.0;
      scan.temperature = sparms.temperature;
      scan.wavelength  = 0.0;
      scan.plateau     = 0.0;
      scan.delta_r     = dr;
      scan.rvalues     = QVector<double>( nrad, 0.0 );
      grid.scanData << scan;
   }

   return grid;
}

// The preparation below is US_2dsaProcess's (cosedimenting branch), in order:
// split the components into the overlay and the buffer below it, sum the
// base (non-sedimenting, non-diffusing) components into a base density and
// viscosity, simulate each co-sedimenting component on the experiment's grid,
// and build the band-forming solution for co-diffusing ones.
bool US_FvmGradients::prepare( const US_Buffer&               buffer,
                               const US_SimulationParameters& sparms,
                               const US_DataIO::RawData&      grid,
                               QString&                       msg )
{
   msg = QString();
   cosed_.reset();
   bfg_.reset();
   components     = buffer.cosed_component;
   base_density   = 0.0;
   base_viscosity = 0.0;
   dens_lo        = buffer.density;
   dens_hi        = buffer.density;

   if ( !forms_gradient( buffer ) )
   {
      return true;
   }

   if ( grid.scanData.size() < 2 || grid.xvalues.size() < 2 )
   {
      msg = QObject::tr( "A gradient needs an experiment grid of at least two scans and two radii." );
      return false;
   }

   // A component present both in the overlay and below it counts by its
   // excess on whichever side holds more.
   QMap<QString, US_CosedComponent> upper;
   QMap<QString, US_CosedComponent> lower;

   for ( const US_CosedComponent& cc : components )
   {
      if ( !cc.overlaying && upper.contains( cc.name ) )
      {
         US_CosedComponent other = upper[cc.name];
         if ( other.conc > cc.conc )
         {
            other.conc     = other.conc - cc.conc;
            upper[cc.name] = other;
         }
         else if ( std::fabs( other.conc - cc.conc ) < GSL_ROOT5_DBL_EPSILON )
         {
            upper.remove( cc.name );
         }
         else
         {
            other.conc     = cc.conc - other.conc;
            lower[cc.name] = other;
            upper.remove( cc.name );
         }
         continue;
      }

      if ( cc.overlaying && lower.contains( cc.name ) )
      {
         US_CosedComponent other = lower[cc.name];
         if ( other.conc > cc.conc )
         {
            other.conc     = other.conc - cc.conc;
            lower[cc.name] = other;
         }
         else if ( std::fabs( other.conc - cc.conc ) < GSL_ROOT5_DBL_EPSILON )
         {
            lower.remove( cc.name );
         }
         else
         {
            other.conc     = cc.conc - other.conc;
            upper[cc.name] = other;
            lower.remove( cc.name );
         }
         continue;
      }

      ( cc.overlaying ? upper : lower )[cc.name] = cc;
   }

   // The buffer editor gives every component the base buffer's density and
   // viscosity as its constant coefficient, so the base is their mean, as
   // US_Math_BF takes it.  (The cosedimenting 2DSA sums them, which is right
   // only for a single base component.)
   int nbase = 0;
   for ( const US_CosedComponent& cc : components )
   {
      if ( !cc.overlaying && lower.contains( cc.name ) && std::fabs( lower[cc.name].conc - cc.conc ) < GSL_ROOT5_DBL_EPSILON &&
           cc.s_coeff == 0.0 && cc.d_coeff == 0.0 )
      {
         base_density += cc.dens_coeff[0];
         base_viscosity += cc.visc_coeff[0];
         nbase++;
      }
   }

   if ( nbase > 0 )
   {
      base_density /= nbase;
      base_viscosity /= nbase;
   }

   if ( base_density <= 0.0 || base_viscosity <= 0.0 )
   {
      msg = QObject::tr( "The buffer has no base component (one that neither sediments nor diffuses), so the gradient"
                         " has nothing to build on." );
      return false;
   }

   US_Math2::SolutionData sd{};
   sd.density   = base_density;
   sd.viscosity = base_viscosity;
   sd.manual    = true;

   bool                              codiff = false;
   US_Model                          cosed_model;
   QMap<QString, US_DataIO::RawData> comp_data;
   US_DataIO::RawData                work = grid;

   for ( US_CosedComponent cc : components )
   {
      if ( cc.overlaying && upper.contains( cc.name ) )
      {
         cc = upper.value( cc.name );
      }
      else if ( !cc.overlaying && lower.contains( cc.name ) )
      {
         cc = lower.value( cc.name );
      }
      else
      {
         continue;
      }

      if ( cc.s_coeff == 0.0 && cc.d_coeff == 0.0 )
      {
         continue;
      }

      if ( cc.s_coeff == 0.0 )
      {
         codiff = true;
         continue;
      }

      US_Model::SimulationComponent comp;
      comp.name                 = cc.name;
      comp.analyteGUID          = cc.GUID;
      comp.molar_concentration  = cc.conc;
      comp.signal_concentration = cc.conc;
      comp.vbar20               = cc.vbar;
      if ( comp_data.contains( comp.analyteGUID ) )
      {
         continue;
      }
      sd.vbar20 = cc.vbar;
      sd.vbar   = cc.vbar;
      US_Math2::data_correction( sparms.temperature, sd );
      comp.s            = cc.s_coeff / sd.s20w_correction;
      comp.D            = cc.d_coeff / sd.D20w_correction;
      comp.f_f0         = 0.0;
      comp.analyte_type = 4;
      cosed_model.components << comp;

      US_Model one;
      one.coSedSolute = -1;
      one.components << comp;
      one.update_coefficients();

      cosed_.reset( new US_LammAstfvm::CosedData( one, sparms, &work, &components, base_density, base_viscosity ) );
      comp_data[comp.analyteGUID] = cosed_->sa_data;
      cosed_model.update_coefficients();
      cosed_->model           = cosed_model;
      cosed_->cosed_comp_data = comp_data;
   }

   if ( cosed_ && !comp_data.isEmpty() )
   {
      cosed_->sa_data = comp_data.first();
   }

   if ( codiff )
   {
      bfg_.reset( new US_Math_BF::Band_Forming_Gradient(
         sparms.meniscus, sparms.bottom, sparms.band_volume, components, sparms.cp_pathlen, sparms.cp_angle ) );
      bfg_->get_eigenvalues();
      bfg_->calculate_gradient( sparms, &work );
   }

   measure_span( buffer, grid );

   if ( !std::isfinite( dens_lo ) || !std::isfinite( dens_hi ) )
   {
      msg = QObject::tr( "The prepared gradient is not finite.  Check the buffer's co-sedimenting components and the"
                         " run's speed profile." );
      return false;
   }

   return true;
}

void US_FvmGradients::measure_span( const US_Buffer& buffer, const US_DataIO::RawData& grid )
{
   const int       npts = grid.xvalues.size();
   QVector<double> xx   = grid.xvalues;
   QVector<double> dens( npts );
   QVector<double> visc( npts );

   dens_lo = 1.0e+99;
   dens_hi = -1.0e+99;

   for ( const US_DataIO::Scan& scan : grid.scanData )
   {
      // As US_LammAstfvm::AdjustSD forms them: the buffer's own values, then
      // each gradient's increment.
      dens.fill( buffer.density );
      visc.fill( buffer.viscosity );

      if ( cosed_ )
      {
         cosed_->InterpolateCCosed( npts, xx.data(), scan.seconds, dens.data(), visc.data() );
      }

      if ( bfg_ )
      {
         bfg_->interpolateCCodiff( npts, xx.data(), scan.seconds, scan.temperature + K0, dens.data(), visc.data() );
      }

      for ( double dd : dens )
      {
         dens_lo = qMin( dens_lo, dd );
         dens_hi = qMax( dens_hi, dd );
      }
   }
}
