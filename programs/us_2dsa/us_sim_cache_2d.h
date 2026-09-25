//! \file us_sim_cache_2d.h
#ifndef US_SIM_CACHE_2D_H
#define US_SIM_CACHE_2D_H

#include <QtCore>
#include <atomic>

#include "us_solve_sim.h"

//! \brief Cache of single-solute simulations for a 2DSA fit.
//!
//! A simulation depends only on the solute's component values and the
//! simulation parameters, not on the data values. Within one fit the grid
//! solutes are simulated again and again (merge depths, refinement
//! iterations, Monte Carlo passes), so each can be reused. Simulations are
//! stored as floats, keyed by the component values plus meniscus, bottom and
//! data dimensions. When the memory cap is reached, further simulations are
//! computed but not stored. Thread-safe.
class US_SimCache2D : public US_SolveSim::SimCache
{
   public:
      //! \brief Create a cache
      //! \param max_mb  Memory cap in megabytes
      US_SimCache2D( qint64 );

      bool fetch( const US_Model::SimulationComponent&,
                  const US_SimulationParameters&,
                  US_DataIO::RawData& ) override;

      void store( const US_Model::SimulationComponent&,
                  const US_SimulationParameters&,
                  const US_DataIO::RawData& ) override;

      //! \brief Text of the cache statistics
      QString stats( void );

   private:
      QByteArray make_key( const US_Model::SimulationComponent&,
                           const US_SimulationParameters&,
                           const US_DataIO::RawData& );

      QHash< QByteArray, QVector< float > > sims;
      QReadWriteLock        lock;
      qint64                max_bytes;
      qint64                nbytes;
      std::atomic< qint64 > nhits;
      std::atomic< qint64 > nmiss;
      qint64                nstored;
      qint64                nfull;
};
#endif
