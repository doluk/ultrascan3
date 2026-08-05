//! \file us_solvent_exchange.cpp

#include <cmath>

#include "us_solvent_exchange.h"

US_SolventExchange::US_SolventExchange()
{
   type        = NONE;
   kinetics    = FIRST_ORDER;
   ligand      = QString();
   sites       = 0.0;
   dmass       = DMASS_H2D;
   vbar_bound  = 0.0;
   rate        = 0.0;
   k_d         = 1.0;
   ligand_ref  = 1.0;
   ligand_conc = 0.0;
   theta0      = 0.0;
}

bool US_SolventExchange::operator== ( const US_SolventExchange& se ) const
{
   if ( type        != se.type        ) return false;
   if ( kinetics    != se.kinetics    ) return false;
   if ( ligand      != se.ligand      ) return false;
   if ( sites       != se.sites       ) return false;
   if ( dmass       != se.dmass       ) return false;
   if ( vbar_bound  != se.vbar_bound  ) return false;
   if ( rate        != se.rate        ) return false;
   if ( k_d         != se.k_d         ) return false;
   if ( ligand_ref  != se.ligand_ref  ) return false;
   if ( ligand_conc != se.ligand_conc ) return false;
   if ( theta0      != se.theta0      ) return false;

   return true;
}

bool US_SolventExchange::active( void ) const
{
   return ( type != NONE  &&  sites > 0.0  &&  dmass != 0.0 );
}

bool US_SolventExchange::preequilibrated( void ) const
{
   return ( theta0 < 0.0 );
}

double US_SolventExchange::ligand_level( const double lig ) const
{
   const double lref = ( ligand_ref > 0.0 ) ? ligand_ref : 1.0;
   const double lvl  = lig / lref;

   return ( lvl > 0.0 ) ? lvl : 0.0;
}

double US_SolventExchange::occupancy_eq( const double lig ) const
{
   const double lvl = ligand_level( lig );

   if ( type == ISOTOPE )
   {  // fraction of the exchangeable solvent pool that carries the isotope,
      // weighted by the fractionation factor k_d
      const double frac  = qMin( lvl, 1.0 );
      const double phi   = ( k_d > 0.0 ) ? k_d : 1.0;
      const double denom = phi * frac + ( 1.0 - frac );

      return ( denom > 0.0 ) ? ( phi * frac / denom ) : 0.0;
   }

   if ( type == BINDING )
   {  // Langmuir isotherm with dissociation constant k_d
      const double kd = ( k_d > 0.0 ) ? k_d : 0.0;

      if ( ( kd + lvl ) <= 0.0 )
      {
         return 0.0;
      }

      return lvl / ( kd + lvl );
   }

   return 0.0;
}

double US_SolventExchange::rate_obs( const double lig ) const
{
   if ( rate <= 0.0 )
   {
      return 0.0;
   }

   const double lvl = ligand_level( lig );

   if ( type == ISOTOPE )
   {  // k_obs = k_ex * ( phi * f + ( 1 - f ) ) so that the forward and backward
      // rates are consistent with the fractionation factor
      const double frac = qMin( lvl, 1.0 );
      const double phi  = ( k_d > 0.0 ) ? k_d : 1.0;

      return rate * ( phi * frac + ( 1.0 - frac ) );
   }

   if ( type == BINDING )
   {  // k_obs = k_on * L + k_off  with  k_on = k_off / k_d
      const double kd = ( k_d > 0.0 ) ? k_d : 0.0;

      if ( kd <= 0.0 )
      {  // no dissociation constant given: binding is not reversible
         return rate;
      }

      return rate * ( 1.0 + lvl / kd );
   }

   return rate;
}

double US_SolventExchange::relax( const double theta, const double lig,
                                  const double dtime ) const
{
   const double theta_eq = occupancy_eq( lig );

   if ( kinetics == EQUILIBRIUM )
   {  // instantaneous local equilibrium
      return theta_eq;
   }

   const double k_obs = rate_obs( lig );

   if ( k_obs <= 0.0  ||  dtime <= 0.0 )
   {  // frozen occupancy
      return theta;
   }

   // the linear relaxation is integrated analytically, so any rate is stable
   const double expon = k_obs * dtime;
   const double decay = ( expon > 700.0 ) ? 0.0 : exp( -expon );

   return theta_eq + ( theta - theta_eq ) * decay;
}

double US_SolventExchange::mass_ratio( const double theta, const double mw ) const
{
   if ( ! active()  ||  mw <= 0.0 )
   {
      return 1.0;
   }

   return 1.0 + theta * sites * dmass / mw;
}

double US_SolventExchange::buoyancy( const double theta, const double rho,
                                     const double mw,    const double vbar ) const
{
   const double buoy = 1.0 - vbar * rho;

   if ( ! active()  ||  mw <= 0.0 )
   {
      return buoy;
   }

   // excess mass fraction of the analyte at the given occupancy
   const double excess = theta * sites * dmass / mw;

   return buoy + excess * ( 1.0 - vbar_bound * rho );
}

double US_SolventExchange::friction_ratio( const double theta, const double mw,
                                           const double vbar ) const
{
   if ( ! active()  ||  mw <= 0.0  ||  vbar <= 0.0  ||  vbar_bound == 0.0 )
   {  // exchanged mass without volume leaves the friction unchanged
      return 1.0;
   }

   // relative volume of the analyte at the given occupancy
   const double vratio = 1.0 + theta * sites * dmass * vbar_bound / ( mw * vbar );

   if ( vratio <= 0.0 )
   {
      return 1.0;
   }

   return cbrt( vratio );
}

void US_SolventExchange::read_attributes( const QXmlStreamAttributes& attrs )
{
   if ( ! attrs.hasAttribute( "exch_type" ) )
   {  // no exchange description present: keep the defaults
      return;
   }

   type            = attrs.value( "exch_type"     ).toString().toInt();
   ligand          = attrs.value( "exch_ligand"   ).toString();

   QString akinet  = attrs.value( "exch_kinetics" ).toString();
   QString asites  = attrs.value( "exch_sites"    ).toString();
   QString admass  = attrs.value( "exch_dmass"    ).toString();
   QString avbarb  = attrs.value( "exch_vbar"     ).toString();
   QString arate   = attrs.value( "exch_rate"     ).toString();
   QString akd     = attrs.value( "exch_kd"       ).toString();
   QString alref   = attrs.value( "exch_ref"      ).toString();
   QString alconc  = attrs.value( "exch_conc"     ).toString();
   QString atheta  = attrs.value( "exch_theta0"   ).toString();

   kinetics    = akinet.isEmpty() ? FIRST_ORDER : akinet.toInt();
   sites       = asites.isEmpty() ? 0.0         : asites.toDouble();
   dmass       = admass.isEmpty() ? DMASS_H2D   : admass.toDouble();
   vbar_bound  = avbarb.isEmpty() ? 0.0         : avbarb.toDouble();
   rate        = arate .isEmpty() ? 0.0         : arate .toDouble();
   k_d         = akd   .isEmpty() ? 1.0         : akd   .toDouble();
   ligand_ref  = alref .isEmpty() ? 1.0         : alref .toDouble();
   ligand_conc = alconc.isEmpty() ? 0.0         : alconc.toDouble();
   theta0      = atheta.isEmpty() ? 0.0         : atheta.toDouble();
}

void US_SolventExchange::write_attributes( QXmlStreamWriter& xml ) const
{
   if ( type == NONE )
   {  // do not clutter models that do not use an exchange
      return;
   }

   // the default precision of QString::number() would round the molar mass
   // increments and rate constants to six digits
   constexpr int prec = 12;

   xml.writeAttribute( "exch_type",     QString::number( type ) );
   xml.writeAttribute( "exch_kinetics", QString::number( kinetics ) );
   xml.writeAttribute( "exch_sites",    QString::number( sites,       'g', prec ) );
   xml.writeAttribute( "exch_dmass",    QString::number( dmass,       'g', prec ) );
   xml.writeAttribute( "exch_vbar",     QString::number( vbar_bound,  'g', prec ) );
   xml.writeAttribute( "exch_rate",     QString::number( rate,        'g', prec ) );
   xml.writeAttribute( "exch_kd",       QString::number( k_d,         'g', prec ) );
   xml.writeAttribute( "exch_ref",      QString::number( ligand_ref,  'g', prec ) );
   xml.writeAttribute( "exch_conc",     QString::number( ligand_conc, 'g', prec ) );
   xml.writeAttribute( "exch_theta0",   QString::number( theta0,      'g', prec ) );

   if ( ! ligand.isEmpty() )
   {
      xml.writeAttribute( "exch_ligand", ligand );
   }
}

QString US_SolventExchange::typeText( void ) const
{
   if ( type == ISOTOPE )
   {
      return QObject::tr( "isotope exchange" );
   }

   if ( type == BINDING )
   {
      return QObject::tr( "ligand binding" );
   }

   return QObject::tr( "none" );
}

void US_SolventExchange::debug( void ) const
{
   qDebug() << "  exchange type"  << type << typeText() << "kinetics" << kinetics;
   qDebug() << "    ligand"       << ligand << "conc" << ligand_conc
            << "ref" << ligand_ref;
   qDebug() << "    sites"        << sites << "dmass" << dmass
            << "vbar_bound" << vbar_bound;
   qDebug() << "    rate"         << rate << "k_d" << k_d << "theta0" << theta0;
}
