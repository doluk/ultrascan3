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
   buf_density    = 0.0;
   buf_viscosity  = 0.0;
}

US_FvmGradients::US_FvmGradients( const US_FvmGradients& other )
{
   components     = other.components;
   base_density   = other.base_density;
   base_viscosity = other.base_viscosity;
   dens_lo        = other.dens_lo;
   dens_hi        = other.dens_hi;
   buf_density    = other.buf_density;
   buf_viscosity  = other.buf_viscosity;
   measured_      = other.measured_;

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
   // gradient, so the first scan comes as soon as the rotor is at speed: a
   // run that rested first would otherwise see a gradient formed during the
   // rest.
   const double t_first =
      sparms.sim_speed_prof.isEmpty() ? t_end / nscans : qMin( (double) sparms.sim_speed_prof[0].time_e_accel + 1.0, t_end );

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
   buf_density    = buffer.density;
   buf_viscosity  = buffer.viscosity;
   density_cache.clear();
   measured_ = false;

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

   // The former is simulated with ASTFEM, which takes the cell's bottom
   // from simparams only when it lies beyond the centerpiece's bottom at
   // rest; otherwise it puts the bottom at the centerpiece's (7.2 cm unless
   // the hardware says otherwise).  The finite-volume solver always takes
   // simparams' bottom, so a cell whose recorded bottom lies short of that
   // would have its former pile up past the solute's bottom, and the
   // gradient lose that much of its mass.  Place the centerpiece's bottom
   // so that ASTFEM's bottom at speed is the solver's.
   US_SimulationParameters fem_parms = sparms;
   {
      const int    speed        = fem_parms.speed_step.isEmpty() ? 0 : (int) fem_parms.speed_step[0].rotorspeed;
      const double stretch      = fem_parms.rotorcoeffs[0] * speed + fem_parms.rotorcoeffs[1] * sq( (double) speed );
      fem_parms.bottom_position = fem_parms.bottom - stretch;
   }

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

      cosed_.reset( new US_LammAstfvm::CosedData( one, fem_parms, &work, &components, base_density, base_viscosity ) );
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

   measure_span( grid );

   if ( !std::isfinite( dens_lo ) || !std::isfinite( dens_hi ) )
   {
      msg = QObject::tr( "The prepared gradient is not finite.  Check the buffer's co-sedimenting components and the"
                         " run's speed profile." );
      return false;
   }

   return true;
}

bool US_FvmGradients::prepare_measured( const US_Buffer&          buffer,
                                        const QString&            former,
                                        const US_DataIO::RawData& molar,
                                        QString&                  msg )
{
   msg = QString();
   cosed_.reset();
   bfg_.reset();
   components     = buffer.cosed_component;
   base_density   = buffer.density;
   base_viscosity = buffer.viscosity;
   buf_density    = buffer.density;
   buf_viscosity  = buffer.viscosity;
   density_cache.clear();
   measured_ = false;

   const US_CosedComponent* cc = nullptr;
   for ( const US_CosedComponent& comp : components )
   {
      if ( comp.name.compare( former, Qt::CaseInsensitive ) == 0 )
      {
         cc = &comp;
         break;
      }
   }

   if ( cc == nullptr )
   {
      msg = QObject::tr( "The buffer has no component \"%1\" to take the measured gradient for." ).arg( former );
      return false;
   }

   if ( cc->s_coeff == 0.0 )
   {
      msg = QObject::tr( "List \"%1\" as co-sedimenting (non-zero s) for a measured gradient: as co-diffusing,"
                         " the solver would build a band-forming gradient of its own." )
               .arg( former );
      return false;
   }

   if ( molar.scanData.size() < 2 || molar.xvalues.size() < 2 )
   {
      msg = QObject::tr( "A measured gradient needs at least two scans of two radii." );
      return false;
   }

   US_Model                      model;
   US_Model::SimulationComponent comp;
   comp.name                 = cc->name;
   comp.analyteGUID          = cc->GUID;
   comp.molar_concentration  = cc->conc;
   comp.signal_concentration = cc->conc;
   comp.vbar20               = cc->vbar;
   comp.s                    = cc->s_coeff;
   comp.D                    = cc->d_coeff;
   comp.analyte_type         = 4;
   model.components << comp;

   cosed_.reset( new US_LammAstfvm::CosedData( model, molar, &components ) );
   measured_ = true;
   measure_span( molar );

   if ( !std::isfinite( dens_lo ) || !std::isfinite( dens_hi ) )
   {
      msg = QObject::tr( "The measured gradient gives no finite density." );
      return false;
   }

   return true;
}

void US_FvmGradients::fill_density( double t, double temp, const QVector<double>& x, QVector<double>& dens, QVector<double>& visc )
{
   const int npts = x.size();

   // As US_LammAstfvm::AdjustSD forms them: the buffer's own values, then
   // each gradient's increment
   dens.fill( buf_density, npts );
   visc.fill( buf_viscosity, npts );
   const double* xx = x.constData();

   if ( cosed_ )
   {
      cosed_->InterpolateCCosed( npts, xx, t, dens.data(), visc.data() );
   }

   if ( bfg_ )
   {
      bfg_->interpolateCCodiff( npts, xx, t, temp + K0, dens.data(), visc.data() );
   }
}

const QVector<double>& US_FvmGradients::density_at( double t, double temp, const QVector<double>& x )
{
   const qint64 key = qRound64( t * 1000.0 );
   auto         it  = density_cache.find( key );

   if ( it == density_cache.end() || it->size() != x.size() )
   {
      QVector<double> dens;
      QVector<double> visc;
      fill_density( t, temp, x, dens, visc );
      it = density_cache.insert( key, dens );
   }

   return it.value();
}

void US_FvmGradients::measure_span( const US_DataIO::RawData& grid )
{
   QVector<double> dens;
   QVector<double> visc;

   dens_lo = 1.0e+99;
   dens_hi = -1.0e+99;

   for ( const US_DataIO::Scan& scan : grid.scanData )
   {
      fill_density( scan.seconds, scan.temperature, grid.xvalues, dens, visc );

      for ( double dd : dens )
      {
         dens_lo = qMin( dens_lo, dd );
         dens_hi = qMax( dens_hi, dd );
      }
   }
}
