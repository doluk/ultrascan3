//! \file us_mpi_sim_cache.h
#ifndef US_MPI_SIM_CACHE_H
#define US_MPI_SIM_CACHE_H

#include <mpi.h>

#include "us_sim_cache.h"

//! \brief Simulation cache in memory shared by the MPI ranks of a node
//!
//! One copy per node replaces a copy per process:  every rank of the node
//! (master, workers, parallel-masters supervisor) reads and adds simulations
//! in an MPI-3 shared-memory window allocated by the node's first rank.
//!
//! Layout of the window:  a header, a table of slots, and a data area of
//! simulations serialized with US_SimCache::pack (so they are restored
//! bit-exactly).  A slot is found by open addressing on the SHA-256 of the
//! simulation key and filled lock-free:  a rank claims an empty slot with a
//! compare-and-swap, writes the key digest, appends the data and then marks
//! the slot ready.  A slot being filled by another rank counts as a miss.
//!
//! Two policies decide what is kept once the cache is full.  The survivor
//! flag of store() is ignored by both:  in a GA nearly every solute of a
//! gene has a nonzero concentration, so the flag does not single out the
//! simulations worth keeping.
//!
//! KEEP_FIRST (2DSA, PCSA):  simulations are kept in the order they arrive;
//! when the data area or the table is full, further simulations are
//! computed but not stored.  These methods repeat the same sequence of
//! solutes (refinement and Monte Carlo iterations, meniscus and bottom
//! points), and for a cyclic sequence longer than the cache keeping its first
//! part reuses a share capacity/sequence of it, while discarding the oldest
//! simulations would reuse none.
//!
//! RECENT (GA):  the cache is split into two generations of half the size.
//! Simulations go into the current generation; a hit in the previous one is
//! copied into the current one.  When the current generation is full, the
//! previous one is emptied and becomes current.  The cache so holds the
//! simulations used most recently, which is what a GA population, drifting
//! toward the fit, reuses.  A rank empties a generation only when no rank
//! reads or writes it:  ranks announce their use of a generation in its
//! user count, and leave a generation being emptied alone.
//!
//! The constructor and destructor are collective over the node communicator.
//! clear() may only be called while no other rank of the node uses the cache.
class US_MPI_NodeSimCache : public US_SimCache
{
   public:
      //! \brief What to keep once the cache is full
      enum Policy
      {
         KEEP_FIRST = 0, //!< Keep the first simulations; store no more when full
         RECENT     = 1  //!< Keep the most recently used simulations
      };

      //! \brief Create the cache (collective over node_comm)
      //! \param node_comm  Communicator of the ranks of one node
      //! \param max_bytes  Capacity of the data area in bytes
      //! \param sim_bytes  Expected bytes of one serialized simulation, to size
      //!                   the slot table
      //! \param policy     What to keep once the cache is full (the same on
      //!                   every rank of the node)
      US_MPI_NodeSimCache( MPI_Comm node_comm, qint64 max_bytes, qint64 sim_bytes, Policy policy = KEEP_FIRST );

      //! \brief Free the shared window (collective over the node communicator)
      ~US_MPI_NodeSimCache() override;

      //! \brief Flag whether the shared window could be allocated on every rank
      //!        of the node (the cache must not be used otherwise)
      bool valid() const
      {
         return base != nullptr;
      }

      bool  fetch( const QByteArray& key, US_DataIO::RawData& simdat ) override;
      bool  store( const QByteArray& key, const US_DataIO::RawData& simdat, bool survivor ) override;
      void  clear() override;
      Stats stats() const override;

      //! \brief Policy of the cache
      Policy policy() const
      {
         return keep;
      }

      //! \brief Turn storing on or off for this rank (fetching stays on).
      //!        Off for evaluations unlikely to be repeated, e.g. the
      //!        off-grid points of the GA gradient search.
      void set_store_enabled( bool enabled )
      {
         store_on = enabled;
      }

      //! \brief Counts of the whole node:  this rank's fetch and store counts
      //!        summed over the node's ranks (collective over node_comm)
      Stats node_stats() const;

   private:
      enum SlotState
      {
         EMPTY   = 0, // Never used
         CLAIMED = 1, // Taken by a rank; digest not yet written
         FILLING = 2, // Digest written; data being written
         READY   = 3, // Simulation stored
         DEAD    = 4  // Digest written, but no room for the data
      };

      class Header;
      class Generation;
      class Slot;

      // Slot of a key digest in generation gg:  the slot holding it, or else
      //  the first empty slot of its probe sequence (null if the table is full)
      Slot*    find( int gg, const QByteArray& digest, bool& found );

      // Get or store a simulation in generation gg (entered by this rank)
      bool     fetch_in( int gg, const QByteArray& digest, US_DataIO::RawData& simdat );
      int      store_in( int gg, const QByteArray& digest, const US_DataIO::RawData& simdat );

      // Announce and end this rank's use of generation gg.  enter() fails
      //  while the generation is being emptied.
      bool     enter( int gg );
      void     leave( int gg );

      // Empty the previous generation and make it current, if generation gg
      //  is still current and no other rank does so already
      void     rotate( int gg );

      // Empty generation gg (no rank may use it)
      void     empty( int gg );

      MPI_Comm comm;      // Node communicator
      MPI_Win  window;    // Shared window
      char*    base;      // Start of the window in this process
      Header*  header;    // Shared header
      Slot*    table;     // Shared slot table
      char*    data;      // Shared data area
      qint64   nslots;    // Number of slots
      qint64   data_size; // Bytes of the data area
      Policy   keep;      // Policy
      int      ngens;     // Generations:  1 (KEEP_FIRST) or 2 (RECENT)
      qint64   gen_slots; // Slots of a generation
      qint64   gen_size;  // Bytes of the data area of a generation
      bool     store_on;  // Flag:  this rank stores simulations

      // Counts of this rank
      qint64   rank_hits;
      qint64   rank_misses;
      qint64   rank_stored;
      qint64   rank_rejected;
      qint64   rank_evicted;
};

#endif
