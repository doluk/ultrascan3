//! \file us_fvm_gradients.cpp
#include "us_fvm_gradients.h"
#include "us_constants.h"
#include "us_math2.h"

#include <algorithm>
#include <cmath>
#include <numeric>

// A former's molar concentration over the run, simulated by the ASTFVM: the
// solver, cell and clock of the solutes it will carry.  Its own
// concentration sets the solution's density and viscosity through its
// component's coefficients, and its s and D follow them.
// The ASTFVM reads the speed profile second by second, so a rest or a slow
// start is followed as recorded, and its cell is simparams' meniscus and
// bottom.  The times start with the uniform loading at zero, then run from
// the rotor's start to the grid's last scan, closer together early, where
// the gradient changes fastest.
static void simulate_former( const US_Model::SimulationComponent& comp,
                             const US_SimulationParameters&       sparms,
                             const US_DataIO::RawData&            grid,
                             double                               base_density,
                             double                               base_viscosity,
                             bool                                 overlaying,
                             const US_CosedComponent&             own,
                             US_DataIO::RawData&                  molar )
{
   QVector<double> rpm;
   QVector<double> w2t;

   for ( const US_SimulationParameters::SimSpeedProf& prof : sparms.sim_speed_prof )
   {
      rpm += prof.rpm_timestate;
      w2t += prof.w2t_timestate;
   }

   const int ntimes  = qMin( rpm.size(), w2t.size() );
   double    t_rotor = 0.0; // The rotor's start

   while ( t_rotor < ntimes && rpm[(int) t_rotor] <= 0.0 )
   {
      t_rotor += 1.0;
   }

   if ( t_rotor >= ntimes )
   {
      t_rotor = 0.0;
   }

   const double t_end  = grid.scanData.last().seconds;
   const int    nscans = qMax( 100, grid.scanData.size() );

   molar = grid;
   molar.scanData.clear();

   for ( int kk = 1; kk <= nscans; kk++ )
   {
      const double    frac = (double) kk / nscans;
      US_DataIO::Scan scan = grid.scanData[0];
      scan.seconds         = t_rotor + ( t_end - t_rotor ) * frac * frac;
      const int it         = qBound( 0, qRound( scan.seconds ), qMax( 0, ntimes - 1 ) );
      scan.rpm             = ( ntimes > 0 ) ? rpm[it] : sparms.speed_step[0].rotorspeed;
      scan.omega2t         = ( ntimes > 0 ) ? w2t[it] : 0.0;
      scan.temperature     = sparms.temperature;
      scan.rvalues         = QVector<double>( grid.xvalues.size(), 0.0 );
      molar.scanData << scan;
   }

   US_Model model;
   model.coSedSolute = -1;
   model.components << comp;
   model.update_coefficients();
   model.components[0].s = comp.s;
   model.components[0].D = comp.D;

   US_Buffer base;
   base.density     = base_density;
   base.viscosity   = base_viscosity;
   base.manual      = true;
   base.description = QString( "Gradient former's base" );

   // A former in the cell's solution starts uniform; only one in a
   // band-forming lamella starts as the lamella
   US_SimulationParameters fvm_parms = sparms;
   fvm_parms.meshType                = US_SimulationParameters::ASTFVM;
   fvm_parms.band_forming            = sparms.band_forming && overlaying;
   US_LammAstfvm astfvm( model, fvm_parms );
   astfvm.set_buffer( base, nullptr, nullptr );
   astfvm.set_self_gradient( own.dens_coeff, own.visc_coeff );
   astfvm.calculate( molar );

   // A concentration never goes negative; the density polynomials take its
   // square root
   for ( US_DataIO::Scan& scan : molar.scanData )
   {
      for ( double& cc : scan.rvalues )
      {
         cc = qMax( 0.0, cc );
      }
   }

   US_DataIO::Scan start = molar.scanData[0];
   start.seconds         = 0.0;
   start.rpm             = 0.0;
   start.omega2t         = 0.0;
   if ( !fvm_parms.band_forming )
   {
      start.rvalues.fill( comp.signal_concentration );
   }
   molar.scanData.prepend( start );
}

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

      // The buffer gives s and D in standard conditions, as the ASTFVM takes
      // them; it corrects them for the base buffer at the run's temperature
      US_Model::SimulationComponent comp;
      comp.name                 = cc.name;
      comp.analyteGUID          = cc.GUID;
      comp.molar_concentration  = cc.conc;
      comp.signal_concentration = cc.conc;
      comp.vbar20               = cc.vbar;
      comp.s                    = cc.s_coeff;
      comp.D                    = cc.d_coeff;
      comp.f_f0                 = 0.0;
      comp.analyte_type         = 4;
      if ( comp_data.contains( comp.analyteGUID ) )
      {
         continue;
      }
      cosed_model.components << comp;

      US_DataIO::RawData molar;
      simulate_former( comp, sparms, grid, base_density, base_viscosity, cc.overlaying, cc, molar );
      comp_data[comp.analyteGUID] = molar;
   }

   if ( !comp_data.isEmpty() )
   {
      US_Model first;
      first.components << cosed_model.components[0];
      cosed_.reset( new US_LammAstfvm::CosedData( first, comp_data.value( first.components[0].analyteGUID ), &components ) );
      cosed_->model           = cosed_model;
      cosed_->cosed_comp_data = comp_data;
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

// The former's fringes on the fit's scans and radii, before noise
static QVector<double> former_fringes( const US_FvmGradients::FormerFit& fit,
                                       const US_DataIO::RawData&         grid,
                                       double                            s,
                                       double                            D,
                                       double                            sigma,
                                       double                            delta )
{
   US_Model::SimulationComponent comp;
   comp.name                 = QString( "Gradient former" );
   comp.molar_concentration  = fit.molar;
   comp.signal_concentration = fit.molar;
   comp.vbar20               = fit.vbar;
   comp.f_f0                 = 0.0;
   comp.analyte_type         = 4;

   US_Model model;
   model.components << comp;
   model.update_coefficients();
   model.components[0].s     = s;
   model.components[0].D     = D;
   model.components[0].sigma = sigma;
   model.components[0].delta = delta;

   US_Buffer base;
   base.density   = fit.base_density;
   base.viscosity = fit.base_viscosity;
   base.manual    = true;

   US_SimulationParameters sparms = fit.sparms;
   sparms.meshType                = US_SimulationParameters::ASTFVM;
   sparms.band_forming            = false;

   US_DataIO::RawData sim = grid;
   for ( US_DataIO::Scan& scan : sim.scanData )
   {
      scan.rvalues.fill( 0.0 );
   }

   US_LammAstfvm astfvm( model, sparms );
   astfvm.set_buffer( base, nullptr, nullptr );
   if ( fit.self_gradient )
   {
      astfvm.set_self_gradient( fit.dens_coeff, fit.visc_coeff );
   }
   astfvm.calculate( sim );

   QVector<double> out;
   out.reserve( sim.scanData.size() * sim.xvalues.size() );
   for ( const US_DataIO::Scan& scan : sim.scanData )
   {
      for ( double cc : scan.rvalues )
      {
         out << cc * fit.fringes_per_molar;
      }
   }
   return out;
}

// A scans-by-radii table less its row and column means: what is left once
// an offset per scan and a profile common to all scans are removed
static void double_center( QVector<double>& vv, int nscans, int nradii )
{
   QVector<double> row( nscans, 0.0 );
   QVector<double> col( nradii, 0.0 );
   double          all = 0.0;

   for ( int ii = 0; ii < nscans; ii++ )
   {
      for ( int jj = 0; jj < nradii; jj++ )
      {
         const double v = vv[ii * nradii + jj];
         row[ii] += v / nradii;
         col[jj] += v / nscans;
         all += v;
      }
   }

   all /= (double) nscans * nradii;
   for ( int ii = 0; ii < nscans; ii++ )
   {
      for ( int jj = 0; jj < nradii; jj++ )
      {
         vv[ii * nradii + jj] -= row[ii] + col[jj] - all;
      }
   }
}

bool US_FvmGradients::fit_former( FormerFit& fit, QString& msg )
{
   msg                          = QString();
   const US_DataIO::RawData& in = fit.scans;

   if ( fit.molar <= 0.0 || fit.fringes_per_molar <= 0.0 || fit.s <= 0.0 || fit.D <= 0.0 )
   {
      msg = QObject::tr( "A former fit needs a positive loading, fringes per mol/L, and start s and D." );
      return false;
   }

   // The scans and radii to fit, clear of the meniscus and bottom
   US_DataIO::RawData grid = in;
   grid.xvalues.clear();
   grid.scanData.clear();
   QVector<int> radii;

   for ( int jj = 0; jj < in.xvalues.size(); jj += qMax( 1, fit.radial_stride ) )
   {
      const double rr = in.xvalues[jj];
      if ( rr > fit.sparms.meniscus + fit.margin && rr < fit.sparms.bottom - fit.margin )
      {
         radii << jj;
         grid.xvalues << rr;
      }
   }

   for ( int kk = 0; kk < in.scanData.size(); kk += qMax( 1, fit.scan_stride ) )
   {
      US_DataIO::Scan scan = in.scanData[kk];
      scan.rvalues.clear();
      for ( int jj : radii )
      {
         scan.rvalues << in.scanData[kk].rvalues[jj];
      }
      grid.scanData << scan;
   }

   const int nscans = grid.scanData.size();
   const int nradii = grid.xvalues.size();

   if ( nscans < 3 || nradii < 3 )
   {
      msg = QObject::tr( "Too few scans or radii to fit a former to: %1 scans of %2 radii." ).arg( nscans ).arg( nradii );
      return false;
   }

   QVector<double> data;
   data.reserve( nscans * nradii );
   for ( const US_DataIO::Scan& scan : grid.scanData )
   {
      data += scan.rvalues;
   }
   double_center( data, nscans, nradii );

   fit.evaluations   = 0;
   const auto unpack = []( const QVector<double>& pp, double& s, double& D, double& sigma, double& delta )
   {
      s     = std::exp( pp[0] ) * 1.0e-13;
      D     = std::exp( pp[1] ) * 1.0e-6;
      sigma = pp.size() > 2 ? pp[2] * pp[2] : 0.0;
      delta = pp.size() > 3 ? pp[3] * pp[3] : 0.0;
   };
   const auto residual = [&]( const QVector<double>& pp, QVector<double>* res )
   {
      double s, D, sigma, delta;
      unpack( pp, s, D, sigma, delta );
      QVector<double> model = former_fringes( fit, grid, s, D, sigma, delta );
      double_center( model, nscans, nradii );
      double ss = 0.0;
      for ( int kk = 0; kk < model.size(); kk++ )
      {
         const double ee = data[kk] - model[kk];
         ss += ee * ee;
         if ( res != nullptr )
         {
            ( *res )[kk] = ee;
         }
      }
      fit.evaluations++;
      return std::sqrt( ss / model.size() );
   };

   // Nelder-Mead
   QVector<double> start{ std::log( fit.s * 1.0e13 ), std::log( fit.D * 1.0e6 ) };
   if ( fit.concentration_dependent )
   {
      start << std::sqrt( fit.sigma > 0.0 ? fit.sigma : 0.25 ) << std::sqrt( fit.delta > 0.0 ? fit.delta : 0.25 );
   }

   const int                npar = start.size();
   QVector<QVector<double>> xx( npar + 1, start );
   QVector<double>          yy( npar + 1 );

   for ( int round = 0; round < 2; round++ ) // restart once from the result
   {
      for ( int ii = 0; ii <= npar; ii++ )
      {
         xx[ii] = start;
         if ( ii > 0 )
         {
            xx[ii][ii - 1] += 0.3;
         }
         yy[ii] = residual( xx[ii], nullptr );
      }

      for ( int it = 0; it < 200 * npar; it++ )
      {
         QVector<int> order( npar + 1 );
         std::iota( order.begin(), order.end(), 0 );
         std::sort( order.begin(), order.end(), [&]( int a, int b ) { return yy[a] < yy[b]; } );
         const int best   = order[0];
         const int worst  = order[npar];
         const int second = order[npar - 1];

         double    spread = 0.0;
         for ( int ii = 0; ii <= npar; ii++ )
         {
            for ( int kk = 0; kk < npar; kk++ )
            {
               spread = qMax( spread, std::fabs( xx[ii][kk] - xx[best][kk] ) );
            }
         }
         if ( spread < 1.0e-4 && yy[worst] - yy[best] < 1.0e-7 * yy[best] )
         {
            break;
         }

         QVector<double> cen( npar, 0.0 );
         for ( int ii = 0; ii <= npar; ii++ )
         {
            if ( ii != worst )
            {
               for ( int kk = 0; kk < npar; kk++ )
               {
                  cen[kk] += xx[ii][kk] / npar;
               }
            }
         }
         const auto along = [&]( double tt )
         {
            QVector<double> pp( npar );
            for ( int kk = 0; kk < npar; kk++ )
            {
               pp[kk] = cen[kk] + tt * ( xx[worst][kk] - cen[kk] );
            }
            return pp;
         };

         const QVector<double> refl = along( -1.0 );
         const double          yref = residual( refl, nullptr );
         if ( yref < yy[best] )
         {
            const QVector<double> expd = along( -2.0 );
            const double          yexp = residual( expd, nullptr );
            xx[worst]                  = ( yexp < yref ) ? expd : refl;
            yy[worst]                  = qMin( yexp, yref );
         }
         else if ( yref < yy[second] )
         {
            xx[worst] = refl;
            yy[worst] = yref;
         }
         else
         {
            const QVector<double> cont = along( 0.5 );
            const double          ycon = residual( cont, nullptr );
            if ( ycon < yy[worst] )
            {
               xx[worst] = cont;
               yy[worst] = ycon;
            }
            else
            {
               for ( int ii = 0; ii <= npar; ii++ )
               {
                  if ( ii != best )
                  {
                     for ( int kk = 0; kk < npar; kk++ )
                     {
                        xx[ii][kk] = 0.5 * ( xx[ii][kk] + xx[best][kk] );
                     }
                     yy[ii] = residual( xx[ii], nullptr );
                  }
               }
            }
         }
      }

      start = xx[std::min_element( yy.begin(), yy.end() ) - yy.begin()];
   }

   QVector<double> res( nscans * nradii, 0.0 );
   fit.rmsd = residual( start, &res );
   unpack( start, fit.s, fit.D, fit.sigma, fit.delta );

   fit.residuals = grid;
   for ( int ii = 0; ii < nscans; ii++ )
   {
      fit.residuals.scanData[ii].rvalues = res.mid( ii * nradii, nradii );
   }

   if ( !std::isfinite( fit.rmsd ) )
   {
      msg = QObject::tr( "The former fit did not converge to a finite RMSD." );
      return false;
   }

   return true;
}
