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
//! Simulations are kept in the order they arrive:  when the data area or the
//! table is full, further simulations are computed but not stored.  The
//! survivor flag of store() is therefore not needed and ignored.
//!
//! The constructor and destructor are collective over the node communicator.
//! clear() may only be called while no other rank of the node uses the cache.
class US_MPI_NodeSimCache : public US_SimCache
{
   public:
      //! \brief Create the cache (collective over node_comm)
      //! \param node_comm  Communicator of the ranks of one node
      //! \param max_bytes  Capacity of the data area in bytes
      //! \param sim_bytes  Expected bytes of one serialized simulation, to size
      //!                   the slot table
      US_MPI_NodeSimCache( MPI_Comm node_comm, qint64 max_bytes, qint64 sim_bytes );

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
      class Slot;

      // Slot of a key digest:  the slot holding it, or else the first empty
      //  slot of its probe sequence (null if the table is full)
      Slot*    find( const QByteArray& digest, bool& found );

      MPI_Comm comm;      // Node communicator
      MPI_Win  window;    // Shared window
      char*    base;      // Start of the window in this process
      Header*  header;    // Shared header
      Slot*    table;     // Shared slot table
      char*    data;      // Shared data area
      qint64   nslots;    // Number of slots
      qint64   data_size; // Bytes of the data area

      // Counts of this rank
      qint64   rank_hits;
      qint64   rank_misses;
      qint64   rank_stored;
      qint64   rank_rejected;
};

#endif
