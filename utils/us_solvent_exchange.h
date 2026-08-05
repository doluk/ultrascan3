//! \file us_solvent_exchange.h
#ifndef US_SOLVENT_EXCHANGE_H
#define US_SOLVENT_EXCHANGE_H

#include <QtCore>

#include "us_extern.h"

//! Mass difference between deuterium and protium in g/mol ( 2.014102 - 1.007825 )
#define DMASS_H2D    1.006277
//! Molar mass increment for a bound cesium ion in g/mol
#define DMASS_CS     132.90545
//! Molarity of the exchangeable hydrogen pool of water in mol/l ( ~ 2 * 55.4 / 2 )
#define CONC_WATER   55.4

//! \brief Solvent exchange / ligand binding description of an analyte.
//!
//! An analyte moving through a solvent gradient may exchange part of its own
//! atoms with the solvent (H/D exchange in a H2O/D2O gradient) or reversibly
//! bind a component of the solvent (Cs+ binding to a virus capsid in a CsCl
//! density gradient).  Both processes change the buoyant mass of the analyte
//! and are driven by the composition of the solvent at the current position of
//! the analyte, so they can only be described while the Lamm equation is being
//! solved.
//!
//! The state of the analyte is described by a single occupancy variable
//! \em theta (0 = no site exchanged/occupied, 1 = all sites exchanged/occupied)
//! which is transported with the analyte and relaxes towards the local
//! equilibrium occupancy with a rate that follows from the kinetic model:
//!
//!   d(theta)/dt = k_obs( L ) * ( theta_eq( L ) - theta )
//!
//! with L the local concentration of the exchanging/binding solvent component.
//! Because the relaxation is integrated analytically, arbitrarily fast
//! kinetics (up to instantaneous local equilibrium) can be modeled without a
//! time step restriction.
//!
//! The description is attached to a model component and is evaluated by the
//! AST finite volume solver (\ref US_LammAstfvm), so a simulation has to use
//! the ASTFVM mesh type for the exchange to be modeled.  It is stored with the
//! model as attributes of the analyte element, for example an H/D exchange of
//! a protein with 350 exchangeable hydrogens in a D2O gradient
//!
//! \code
//! <analyte name="protein" mw="50000" vbar20="0.72" s="4.2e-13" D="6.5e-07"
//!          exch_type="1" exch_kinetics="1" exch_ligand="Deuterium oxide"
//!          exch_sites="350" exch_dmass="1.006277" exch_rate="0.01"
//!          exch_ref="55.4" exch_theta0="0"/>
//! \endcode
//!
//! or cesium ions binding to a capsid in a CsCl density gradient
//!
//! \code
//! <analyte name="AAV" mw="3.7e+06" vbar20="0.70" s="1.0e-12" D="1.5e-07"
//!          exch_type="2" exch_kinetics="1" exch_ligand="Cesium chloride"
//!          exch_sites="3915" exch_dmass="132.90545" exch_vbar="0.0"
//!          exch_rate="0.5" exch_kd="0.5" exch_ref="1.0" exch_theta0="-1"/>
//! \endcode
class US_UTIL_EXTERN US_SolventExchange
{
   public:
      //! The process that couples the analyte to the solvent
      enum ExchangeType
      {
         NONE     = 0,  //!< No exchange modeled
         ISOTOPE  = 1,  //!< Isotope (H/D) exchange with the solvent
         BINDING  = 2   //!< Reversible ligand binding (Langmuir isotherm)
      };

      //! The kinetic law used to reach the local equilibrium occupancy
      enum KineticsType
      {
         EQUILIBRIUM = 0, //!< Instantaneous local equilibrium (infinitely fast)
         FIRST_ORDER = 1  //!< First order relaxation with rate \ref rate
      };

      US_SolventExchange();

      //! A test for identical exchange descriptions
      bool operator== ( const US_SolventExchange& ) const;

      //! A test for differing exchange descriptions
      inline bool operator!= ( const US_SolventExchange& se ) const
      { return ! operator==( se ); }

      int     type;        //!< Exchange process (\ref ExchangeType)
      int     kinetics;    //!< Kinetic law (\ref KineticsType)
      QString ligand;      //!< Name or GUID of the co-sedimenting/co-diffusing
                           //!<   buffer component driving the exchange
      double  sites;       //!< Number of exchangeable/bindable sites per molecule
      double  dmass;       //!< Molar mass added per occupied site (g/mol)
      double  vbar_bound;  //!< Partial specific volume of the added mass (ml/g)
      double  rate;        //!< Intrinsic rate constant (1/s); k_ex for isotope
                           //!<   exchange, k_off for ligand binding
      double  k_d;         //!< Isotope fractionation factor (ISOTOPE) or
                           //!<   dissociation constant (BINDING), in units of
                           //!<   the normalized ligand level
      double  ligand_ref;  //!< Ligand concentration that maps to a normalized
                           //!<   ligand level of 1 (mol/l).  Use CONC_WATER for
                           //!<   an H/D exchange in a D2O gradient and 1.0 to
                           //!<   work with molar concentrations directly.
      double  ligand_conc; //!< Uniform ligand concentration (mol/l) used when no
                           //!<   gradient of \ref ligand is available
      double  theta0;      //!< Initial occupancy; a negative value starts the
                           //!<   analyte at the local equilibrium occupancy

      //! \brief Flag whether an exchange should be modeled at all
      bool   active( void ) const;

      //! \brief Flag whether the initial occupancy is the local equilibrium one
      bool   preequilibrated( void ) const;

      //! \brief Normalized ligand level of a local ligand concentration
      //! \param lig Local ligand concentration in mol/l
      //! \returns   Ligand concentration divided by \ref ligand_ref
      double ligand_level( double lig ) const;

      //! \brief Equilibrium occupancy at a local ligand concentration
      //! \param lig Local ligand concentration in mol/l
      //! \returns   Occupancy (0 to 1) reached after infinite time
      double occupancy_eq( double lig ) const;

      //! \brief Observed relaxation rate at a local ligand concentration
      //! \param lig Local ligand concentration in mol/l
      //! \returns   Rate constant of the approach to \ref occupancy_eq in 1/s
      double rate_obs( double lig ) const;

      //! \brief Integrate the kinetics over a time step
      //! \param theta Occupancy at the beginning of the time step
      //! \param lig   Local ligand concentration in mol/l
      //! \param dtime Length of the time step in seconds
      //! \returns     Occupancy at the end of the time step
      double relax( double theta, double lig, double dtime ) const;

      //! \brief Relative molar mass of the analyte at a given occupancy
      //! \param theta Occupancy of the analyte
      //! \param mw    Molar mass of the unexchanged analyte in g/mol
      //! \returns     M(theta)/M(0)
      double mass_ratio( double theta, double mw ) const;

      //! \brief Buoyancy term of the analyte at a given occupancy.
      //! This replaces the ( 1 - vbar * density ) term of the Svedberg
      //! equation and is scaled so that an occupancy of zero reproduces it.
      //! \param theta Occupancy of the analyte
      //! \param rho   Local solvent density in g/ml
      //! \param mw    Molar mass of the unexchanged analyte in g/mol
      //! \param vbar  Partial specific volume of the unexchanged analyte
      //! \returns     M(theta)/M(0) - rho * V(theta)/V(0) * vbar
      double buoyancy( double theta, double rho, double mw, double vbar ) const;

      //! \brief Relative frictional coefficient at a given occupancy,
      //! assuming an unchanged shape and a volume increased by the bound mass
      //! \param theta Occupancy of the analyte
      //! \param mw    Molar mass of the unexchanged analyte in g/mol
      //! \param vbar  Partial specific volume of the unexchanged analyte
      //! \returns     f(theta)/f(0)
      double friction_ratio( double theta, double mw, double vbar ) const;

      //! \brief Read the exchange attributes of a model component
      //! \param attrs Attributes of the analyte element of a model XML
      void read_attributes( const QXmlStreamAttributes& attrs );

      //! \brief Write the exchange attributes of a model component.
      //! Nothing is written for a component without an exchange description.
      //! \param xml Stream positioned inside the analyte element
      void write_attributes( QXmlStreamWriter& xml ) const;

      //! \brief A short text description of the exchange
      QString typeText( void ) const;

      //! \brief Dump the exchange description for debugging
      void debug( void ) const;
};
#endif
